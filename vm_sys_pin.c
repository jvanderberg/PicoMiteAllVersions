#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "vm_sys_pin.h"

#ifdef MMBASIC_HOST

void vm_sys_pin_setpin(int64_t pin, int mode) {
    (void)pin;
    (void)mode;
}

int64_t vm_sys_pin_read(int64_t pin) {
    (void)pin;
    return 0;
}

void vm_sys_pin_write(int64_t pin, int64_t value) {
    (void)pin;
    (void)value;
}

#else

#include "hardware/adc.h"
#include "hardware/gpio.h"

enum {
    VM_PIN_EXT_NOT_CONFIG = 0,
    VM_PIN_EXT_DIG_IN = 2,
    VM_PIN_EXT_DIG_OUT = 8
};

#ifdef rp2350
static const uint8_t vm_pin_gpio_map[48] = {
    1, 2, 4, 5, 6, 7, 9, 10, 11, 12, 14, 15, 16, 17, 19, 20,
    21, 22, 24, 25, 26, 27, 29, 41, 42, 43, 31, 32, 34, 44, 45, 46,
    47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62
};
#else
static const uint8_t vm_pin_gpio_map[30] = {
    1, 2, 4, 5, 6, 7, 9, 10, 11, 12, 14, 15, 16, 17, 19,
    20, 21, 22, 24, 25, 26, 27, 29, 41, 42, 43, 31, 32, 34, 44
};
#endif

extern volatile int ExtCurrentConfig[NBRPINS + 1];
extern uint32_t pinmask;
extern int last_adc;
#ifdef rp2350
extern bool rp2350a;
#endif

static int vm_pin_codemap(int64_t gpio_index) {
#ifdef rp2350
    int max_gpio_index = rp2350a ? 29 : 47;
#else
    int max_gpio_index = 29;
#endif
    if (gpio_index < 0 || gpio_index > max_gpio_index)
        error("Invalid GPIO");
    return vm_pin_gpio_map[(int)gpio_index];
}

static int vm_pin_resolve(int64_t encoded_pin) {
    int pin = 0;
    if (encoded_pin < 0)
        pin = vm_pin_codemap(-encoded_pin - 1);
    else if (encoded_pin <= INT32_MAX)
        pin = (int)encoded_pin;
    else
        error("Invalid pin");

#ifdef rp2350
    if (pin < 1 || pin > (rp2350a ? 44 : NBRPINS))
        error("Invalid pin");
#else
    if (pin < 1 || pin > NBRPINS)
        error("Invalid pin");
#endif
    if (PinDef[pin].mode & UNUSED)
        error("Invalid pin");
    return pin;
}

static void vm_pin_prepare_sio(int pin) {
    gpio_set_function(PinDef[pin].GPno, GPIO_FUNC_SIO);
    gpio_set_input_hysteresis_enabled(PinDef[pin].GPno, true);
}

static void vm_pin_set_low(int pin) {
    gpio_set_pulls(PinDef[pin].GPno, false, false);
    gpio_pull_down(PinDef[pin].GPno);
    gpio_put(PinDef[pin].GPno, GPIO_PIN_RESET);
}

static void vm_pin_set_high(int pin) {
    gpio_set_pulls(PinDef[pin].GPno, false, false);
    gpio_pull_up(PinDef[pin].GPno);
    gpio_put(PinDef[pin].GPno, GPIO_PIN_SET);
}

static void vm_pin_set_input(int pin) {
    gpio_set_dir(PinDef[pin].GPno, GPIO_IN);
    gpio_set_input_enabled(PinDef[pin].GPno, true);
    uSec(2);
}

static void vm_pin_set_output(int pin) {
    gpio_set_dir(PinDef[pin].GPno, GPIO_OUT);
    gpio_set_input_enabled(PinDef[pin].GPno, false);
    gpio_set_drive_strength(PinDef[pin].GPno, GPIO_DRIVE_STRENGTH_8MA);
    uSec(2);
}

static uint32_t vm_pin_mask_bit(int pin) {
    int gp = PinDef[pin].GPno;
    return (gp >= 0 && gp < 32) ? (1u << gp) : 0;
}

void vm_sys_pin_setpin(int64_t encoded_pin, int mode) {
    int pin = vm_pin_resolve(encoded_pin);
    uint32_t bit = vm_pin_mask_bit(pin);

    if (mode != VM_PIN_MODE_OFF && mode != VM_PIN_MODE_DIN && mode != VM_PIN_MODE_DOUT)
        error("Unsupported SETPIN mode");

    gpio_disable_pulls(PinDef[pin].GPno);
    if (ExtCurrentConfig[pin] == VM_PIN_EXT_NOT_CONFIG ||
        ExtCurrentConfig[pin] == VM_PIN_EXT_DIG_IN ||
        ExtCurrentConfig[pin] == VM_PIN_EXT_DIG_OUT) {
        gpio_set_input_enabled(PinDef[pin].GPno, false);
        gpio_deinit(PinDef[pin].GPno);
    } else {
        error("Pin in use");
    }

    if (mode == VM_PIN_MODE_OFF) {
        ExtCurrentConfig[pin] = VM_PIN_EXT_NOT_CONFIG;
        pinmask &= ~bit;
        return;
    }

    if (mode == VM_PIN_MODE_DIN) {
        if (!(PinDef[pin].mode & DIGITAL_IN))
            error("Invalid configuration");
        gpio_init(PinDef[pin].GPno);
        vm_pin_prepare_sio(pin);
        vm_pin_set_input(pin);
        ExtCurrentConfig[pin] = VM_PIN_EXT_DIG_IN;
        pinmask &= ~bit;
        return;
    }

    if (!(PinDef[pin].mode & DIGITAL_OUT))
        error("Invalid configuration");
    gpio_init(PinDef[pin].GPno);
    vm_pin_prepare_sio(pin);
    vm_pin_set_output(pin);
    if (bit && (pinmask & bit))
        gpio_put(PinDef[pin].GPno, GPIO_PIN_SET);
    ExtCurrentConfig[pin] = VM_PIN_EXT_DIG_OUT;
    pinmask &= ~bit;
}

int64_t vm_sys_pin_read(int64_t encoded_pin) {
    int pin = vm_pin_resolve(encoded_pin);
    if (ExtCurrentConfig[pin] == VM_PIN_EXT_DIG_OUT)
        return gpio_get_out_level(PinDef[pin].GPno);
    if (ExtCurrentConfig[pin] == VM_PIN_EXT_DIG_IN)
        return gpio_get(PinDef[pin].GPno);
    error("Pin not configured");
    return 0;
}

void vm_sys_pin_write(int64_t encoded_pin, int64_t value) {
    int pin = vm_pin_resolve(encoded_pin);
    uint32_t bit = vm_pin_mask_bit(pin);

    if (ExtCurrentConfig[pin] == VM_PIN_EXT_NOT_CONFIG) {
        vm_pin_prepare_sio(pin);
        vm_pin_set_output(pin);
        pinmask |= bit;
        last_adc = 99;
    } else if (ExtCurrentConfig[pin] != VM_PIN_EXT_DIG_OUT) {
        error("Pin is not an output");
    }

    if (value)
        vm_pin_set_high(pin);
    else
        vm_pin_set_low(pin);
}

#endif
