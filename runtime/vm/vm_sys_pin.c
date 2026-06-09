/*
 * VM syscall conversion rule:
 * - copy/adapt legacy implementation code as closely as possible
 * - copy/adapt dependent legacy helpers too when needed
 * - do not invent new algorithms when legacy code already exists
 * - do not call, wrap, or dispatch back into legacy handlers
 * Any deviation from legacy implementation shape must be explicit and justified.
 */

/*
 * vm_sys_pin.c — shared impl of the VM's pin syscalls for every real
 * hardware port.
 *
 * Drives SETPIN / PIN-read / PIN-write for digital GPIO, raw ADC, and
 * PWM-slice assignment entirely through hal_pin_*; all target hardware
 * specifics live in each port's hal_pin backend
 * (ports/pico_sdk_common/hal_pin_pico.c, ports/esp32_s3/main/hal_pin_esp32.c).
 * Paired with ports/vm_sys_sim/vm_sys_pin_sim.c (software simulation);
 * the build links exactly one of the two per target.
 *
 * Shared helpers + PWM-slice storage macros live in
 * vm_sys_pin_internal.h (static inline), so both TUs compile their
 * own copies without a cross-file dependency.
 */

#include "vm_sys_pin_internal.h"
#include "hal/hal_pin.h"
#include "i2c_config.h"
#include "uart_config.h"
#include "spi_config.h"

static int vm_pwm_pin_a[VM_PWM_SLICE_COUNT];
static int vm_pwm_pin_b[VM_PWM_SLICE_COUNT];

extern volatile int ExtCurrentConfig[NBRPINS + 1];
extern uint32_t pinmask;
extern int last_adc;
/* rp2350a is unconditionally true on rp2040 (stubbed in PicoMite.c) and
 * false on ports without an RP2 package (e.g. ESP32-S3), so portable
 * code can branch on the package-variant flag without a target gate. */
extern bool rp2350a;

static int vm_pin_resolve(int64_t encoded_pin) {
    int pin = 0;
    if (encoded_pin < 0)
        pin = codemap((int)(-encoded_pin - 1));
    else if (encoded_pin <= INT32_MAX)
        pin = (int)encoded_pin;
    else
        error("Invalid pin");

    /* Pin range: rp2350b has 62 GPIOs (NBRPINS=62 there); rp2040 +
     * rp2350a cap at 44. On ports where rp2350a is false the limit is
     * the port's NBRPINS, so the same condition covers every target. */
    if (pin < 1 || pin > (rp2350a ? 44 : NBRPINS))
        error("Invalid pin");
    if (PinDef[pin].mode & UNUSED)
        error("Invalid pin");
    return pin;
}

static void vm_pin_prepare_sio(int pin) {
    hal_pin_set_function(PinDef[pin].GPno, HAL_PIN_FUNC_SIO);
    hal_pin_set_input_hysteresis(PinDef[pin].GPno, true);
}

static void vm_pin_set_low(int pin) {
    hal_pin_set_pulls(PinDef[pin].GPno, HAL_PIN_PULL_NONE);
    hal_pin_set_pulls(PinDef[pin].GPno, HAL_PIN_PULL_DOWN);
    hal_pin_write(PinDef[pin].GPno, false);
}

static void vm_pin_set_high(int pin) {
    hal_pin_set_pulls(PinDef[pin].GPno, HAL_PIN_PULL_NONE);
    hal_pin_set_pulls(PinDef[pin].GPno, HAL_PIN_PULL_UP);
    hal_pin_write(PinDef[pin].GPno, true);
}

static void vm_pin_set_input(int pin) {
    hal_pin_set_dir(PinDef[pin].GPno, HAL_PIN_DIR_IN);
    hal_pin_set_input_enabled(PinDef[pin].GPno, true);
    uSec(2);
}

static void vm_pin_set_output(int pin) {
    hal_pin_set_dir(PinDef[pin].GPno, HAL_PIN_DIR_OUT);
    hal_pin_set_input_enabled(PinDef[pin].GPno, false);
    hal_pin_set_drive_mA(PinDef[pin].GPno, 8);
    uSec(2);
}

static uint32_t vm_pin_mask_bit(int pin) {
    int gp = PinDef[pin].GPno;
    return (gp >= 0 && gp < 32) ? (1u << gp) : 0;
}

static int vm_pin_is_safe_config(int cfg) {
    return cfg == EXT_NOT_CONFIG ||
           cfg == EXT_DIG_IN ||
           cfg == EXT_DIG_OUT ||
           cfg == EXT_ADCRAW ||
           vm_pin_mode_is_pwm(cfg);
}

static void vm_pin_clear_pwm_assignment(int pin) {
    for (int slice = 0; slice < VM_PWM_SLICE_COUNT; slice++) {
        if (vm_pwm_pin_a[slice] == pin) vm_pwm_pin_a[slice] = 0;
        if (vm_pwm_pin_b[slice] == pin) vm_pwm_pin_b[slice] = 0;
    }
}

static int vm_pin_pwm_mode_valid_for_pin(int pin, int mode) {
    switch (mode) {
    case VM_PIN_MODE_PWM0A:
        return (PinDef[pin].mode & PWM0A) != 0;
    case VM_PIN_MODE_PWM0B:
        return (PinDef[pin].mode & PWM0B) != 0;
    case VM_PIN_MODE_PWM1A:
        return (PinDef[pin].mode & PWM1A) != 0;
    case VM_PIN_MODE_PWM1B:
        return (PinDef[pin].mode & PWM1B) != 0;
    case VM_PIN_MODE_PWM2A:
        return (PinDef[pin].mode & PWM2A) != 0;
    case VM_PIN_MODE_PWM2B:
        return (PinDef[pin].mode & PWM2B) != 0;
    case VM_PIN_MODE_PWM3A:
        return (PinDef[pin].mode & PWM3A) != 0;
    case VM_PIN_MODE_PWM3B:
        return (PinDef[pin].mode & PWM3B) != 0;
    case VM_PIN_MODE_PWM4A:
        return (PinDef[pin].mode & PWM4A) != 0;
    case VM_PIN_MODE_PWM4B:
        return (PinDef[pin].mode & PWM4B) != 0;
    case VM_PIN_MODE_PWM5A:
        return (PinDef[pin].mode & PWM5A) != 0;
    case VM_PIN_MODE_PWM5B:
        return (PinDef[pin].mode & PWM5B) != 0;
    case VM_PIN_MODE_PWM6A:
        return (PinDef[pin].mode & PWM6A) != 0;
    case VM_PIN_MODE_PWM6B:
        return (PinDef[pin].mode & PWM6B) != 0;
    case VM_PIN_MODE_PWM7A:
        return (PinDef[pin].mode & PWM7A) != 0;
    case VM_PIN_MODE_PWM7B:
        return (PinDef[pin].mode & PWM7B) != 0;
    /* PWM8..11 mask bits are unconditional in configuration.h;
         * on rp2040 no PinDef entry ever sets them so these return 0. */
    case VM_PIN_MODE_PWM8A:
        return (PinDef[pin].mode & PWM8A) != 0;
    case VM_PIN_MODE_PWM8B:
        return (PinDef[pin].mode & PWM8B) != 0;
    case VM_PIN_MODE_PWM9A:
        return (PinDef[pin].mode & PWM9A) != 0;
    case VM_PIN_MODE_PWM9B:
        return (PinDef[pin].mode & PWM9B) != 0;
    case VM_PIN_MODE_PWM10A:
        return (PinDef[pin].mode & PWM10A) != 0;
    case VM_PIN_MODE_PWM10B:
        return (PinDef[pin].mode & PWM10B) != 0;
    case VM_PIN_MODE_PWM11A:
        return (PinDef[pin].mode & PWM11A) != 0;
    case VM_PIN_MODE_PWM11B:
        return (PinDef[pin].mode & PWM11B) != 0;
    default:
        return 0;
    }
}

int vm_pin_pwm_assigned_pin(int channel, int which) {
    if (channel < 0 || channel >= VM_PWM_SLICE_COUNT) return 0;
    return which ? vm_pwm_pin_b[channel] : vm_pwm_pin_a[channel];
}

/* Bind a BASIC pin index to the PWM channel output named by `mode` (a
 * VM_PIN_MODE_PWM* / EXT_PWM* value — the two enumerations share the same
 * numeric codes). The interpreter's SETPIN path calls this so the shared
 * PWM syscall can resolve channel→GPIO identically to the bytecode path. */
void vm_pin_pwm_register_setpin(int pin, int mode) {
    int slice = -1, chan = -1;
    if (!vm_pin_mode_is_pwm(mode)) {
        /* SETPIN turned this pin to a non-PWM mode (or OFF); drop any stale
         * channel↔pin assignment so it can no longer resolve as a PWM
         * target. */
        vm_pin_clear_pwm_assignment(pin);
        return;
    }
    vm_pin_pwm_mode_to_slice_chan(mode, &slice, &chan);
    if (slice < 0 || slice >= VM_PWM_SLICE_COUNT) return;
    if (chan == 0)
        vm_pwm_pin_a[slice] = pin;
    else
        vm_pwm_pin_b[slice] = pin;
}

void vm_pin_pwm_mark_reserved(int channel, int which) {
    int pin = vm_pin_pwm_assigned_pin(channel, which);
    if (pin == 0) return;
    ExtCurrentConfig[pin] = EXT_COM_RESERVED;
    hal_pin_set_function(PinDef[pin].GPno, HAL_PIN_FUNC_PWM);
}

void vm_pin_pwm_release(int channel) {
    if (channel < 0 || channel >= VM_PWM_SLICE_COUNT) return;
    if (vm_pwm_pin_a[channel]) {
        hal_pin_deinit(PinDef[vm_pwm_pin_a[channel]].GPno);
        ExtCurrentConfig[vm_pwm_pin_a[channel]] = EXT_NOT_CONFIG;
    }
    if (vm_pwm_pin_b[channel]) {
        hal_pin_deinit(PinDef[vm_pwm_pin_b[channel]].GPno);
        ExtCurrentConfig[vm_pwm_pin_b[channel]] = EXT_NOT_CONFIG;
    }
}

void vm_sys_pin_setpin(int64_t encoded_pin, int mode, int option) {
    int pin = vm_pin_resolve(encoded_pin);
    uint32_t bit = vm_pin_mask_bit(pin);
    int slice = -1, chan = -1;

    if (mode == VM_PIN_MODE_PWM_AUTO)
        mode = vm_pin_pwm_mode_for_auto(pin);

    if (mode == VM_PIN_MODE_OFF) {
        if (ExtCurrentConfig[pin] >= EXT_COM_RESERVED)
            error("Pin in use");
        hal_pin_set_input_enabled(PinDef[pin].GPno, false);
        hal_pin_deinit(PinDef[pin].GPno);
        ExtCurrentConfig[pin] = EXT_NOT_CONFIG;
        vm_pin_clear_pwm_assignment(pin);
        uart_config_clear_pin(pin);
        spi_config_clear_pin(pin);
        i2c_config_clear_pin(pin);
        pinmask &= ~bit;
        return;
    }

    if (i2c_config_mode_is_i2c(mode)) {
        if (option != VM_PIN_OPT_NONE)
            error("Unsupported SETPIN option");
        if (!vm_pin_is_safe_config(ExtCurrentConfig[pin]))
            error("Pin in use");
        i2c_config_setpin(pin, mode);
        vm_pin_clear_pwm_assignment(pin);
        ExtCurrentConfig[pin] = mode;
        return;
    }

    if (uart_config_mode_is_uart(mode)) {
        if (option != VM_PIN_OPT_NONE)
            error("Unsupported SETPIN option");
        if (!vm_pin_is_safe_config(ExtCurrentConfig[pin]))
            error("Pin in use");
        uart_config_setpin(pin, mode);
        vm_pin_clear_pwm_assignment(pin);
        ExtCurrentConfig[pin] = mode;
        return;
    }

    if (spi_config_mode_is_spi(mode)) {
        if (option != VM_PIN_OPT_NONE)
            error("Unsupported SETPIN option");
        if (!vm_pin_is_safe_config(ExtCurrentConfig[pin]))
            error("Pin in use");
        spi_config_setpin(pin, mode);
        vm_pin_clear_pwm_assignment(pin);
        ExtCurrentConfig[pin] = mode;
        return;
    }

    if (option != VM_PIN_OPT_NONE &&
        mode != VM_PIN_MODE_DIN)
        error("Unsupported SETPIN option");

    hal_pin_set_pulls(PinDef[pin].GPno, HAL_PIN_PULL_NONE);
    if (!vm_pin_is_safe_config(ExtCurrentConfig[pin]))
        error("Pin in use");
    hal_pin_set_input_enabled(PinDef[pin].GPno, false);
    hal_pin_deinit(PinDef[pin].GPno);

    if (mode == VM_PIN_MODE_DIN) {
        if (!(PinDef[pin].mode & DIGITAL_IN))
            error("Invalid configuration");
        hal_pin_init_digital(PinDef[pin].GPno);
        vm_pin_prepare_sio(pin);
        if (option == VM_PIN_OPT_PULLUP)
            hal_pin_set_pulls(PinDef[pin].GPno, HAL_PIN_PULL_UP);
        else if (option == VM_PIN_OPT_PULLDOWN)
            hal_pin_set_pulls(PinDef[pin].GPno, HAL_PIN_PULL_DOWN);
        else if (option != VM_PIN_OPT_NONE)
            error("Unsupported SETPIN option");
        vm_pin_clear_pwm_assignment(pin);
        vm_pin_set_input(pin);
        ExtCurrentConfig[pin] = EXT_DIG_IN;
        pinmask &= ~bit;
        return;
    }

    if (mode == VM_PIN_MODE_ARAW) {
        if (!(PinDef[pin].mode & ANALOG_IN))
            error("Invalid configuration");
        if (hal_pin_adc_validate(pin) != 0)
            error("Invalid configuration");
        hal_pin_init_digital(PinDef[pin].GPno);
        hal_pin_set_mode(PinDef[pin].GPno, HAL_PIN_MODE_ANALOG);
        vm_pin_clear_pwm_assignment(pin);
        ExtCurrentConfig[pin] = EXT_ADCRAW;
        pinmask &= ~bit;
        return;
    }

    if (mode == VM_PIN_MODE_DOUT) {
        if (!(PinDef[pin].mode & DIGITAL_OUT))
            error("Invalid configuration");
        hal_pin_init_digital(PinDef[pin].GPno);
        vm_pin_prepare_sio(pin);
        vm_pin_clear_pwm_assignment(pin);
        vm_pin_set_output(pin);
        if (bit && (pinmask & bit))
            hal_pin_write(PinDef[pin].GPno, true);
        ExtCurrentConfig[pin] = EXT_DIG_OUT;
        pinmask &= ~bit;
        return;
    }

    if (!vm_pin_mode_is_pwm(mode) || !vm_pin_pwm_mode_valid_for_pin(pin, mode))
        error("Invalid configuration");
    vm_pin_pwm_mode_to_slice_chan(mode, &slice, &chan);
    if (slice > vm_pwm_max_slice())
        error("Invalid configuration");
    if (chan == 0) {
        if (vm_pwm_pin_a[slice] && vm_pwm_pin_a[slice] != pin)
            error("Already Set to pin %", vm_pwm_pin_a[slice]);
        vm_pwm_pin_a[slice] = pin;
    } else {
        if (vm_pwm_pin_b[slice] && vm_pwm_pin_b[slice] != pin)
            error("Already Set to pin %", vm_pwm_pin_b[slice]);
        vm_pwm_pin_b[slice] = pin;
    }
    hal_pin_init_digital(PinDef[pin].GPno);
    hal_pin_set_function(PinDef[pin].GPno, HAL_PIN_FUNC_PWM);
    ExtCurrentConfig[pin] = mode;
    pinmask &= ~bit;
}

int64_t vm_sys_pin_read(int64_t encoded_pin) {
    int pin = vm_pin_resolve(encoded_pin);
    if (ExtCurrentConfig[pin] == EXT_DIG_OUT)
        return hal_pin_read_output_latch(PinDef[pin].GPno);
    if (ExtCurrentConfig[pin] == EXT_DIG_IN)
        return hal_pin_read(PinDef[pin].GPno);
    if (ExtCurrentConfig[pin] == EXT_ADCRAW) {
        if (ADCDualBuffering || dmarunning) error("ADC in use");
        hal_pin_adc_init();
        hal_pin_adc_select(PinDef[pin].ADCpin);
        last_adc = PinDef[pin].ADCpin;
        return hal_pin_adc_read();
    }
    error("Pin not configured");
    return 0;
}

void vm_sys_pin_write(int64_t encoded_pin, int64_t value) {
    int pin = vm_pin_resolve(encoded_pin);
    uint32_t bit = vm_pin_mask_bit(pin);

    if (ExtCurrentConfig[pin] == EXT_NOT_CONFIG) {
        vm_pin_prepare_sio(pin);
        vm_pin_set_output(pin);
        pinmask |= bit;
        last_adc = 99;
    } else if (ExtCurrentConfig[pin] != EXT_DIG_OUT) {
        error("Pin is not an output");
    }

    if (value)
        vm_pin_set_high(pin);
    else
        vm_pin_set_low(pin);
}

void vm_sys_pin_reset(void) {
    memset(vm_pwm_pin_a, 0, sizeof(vm_pwm_pin_a));
    memset(vm_pwm_pin_b, 0, sizeof(vm_pwm_pin_b));
}
