/*
 * Classic ESP32 GPIO map.
 *
 * BASIC's GPn syntax maps through codemap() into PinDef[] slots. Keep slot
 * zero as the legacy NULL row, then map GP0..GP39 to slots 1..40 so shared
 * pin-state arrays keep their usual 1-based indexing.
 *
 * Classic-ESP32 hardware shape encoded here:
 *  - GPIO 1/3 are the UART0 console and stay reserved.
 *  - GPIO 6..11 are the flash pins and stay reserved.
 *  - GPIO 20, 24, 28..31, 37, 38 are not bonded on WROOM-32 modules.
 *  - GPIO 34..39 are input-only (no output driver, no pull resistors).
 *  - Analog input is advertised on ADC1 pins only (GPIO 32..36, 39):
 *    ADC2 cannot convert while Wi-Fi is active, so ADC2-capable pins are
 *    presented as digital-only rather than as analog pins that stop
 *    working when the radio comes up.
 *  - GPIO 0 is the BOOT strapping pin / boot button: input only here.
 *  - GPIO 2, 5, 12, 15 are strapping pins; they are exposed as normal
 *    GPIOs (CYD wiring uses them) but board profiles should default them
 *    to quiescent roles.
 */

#include <ctype.h>
#include <stdint.h>

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"

#define ESP32_ADC_NONE 99
#define ESP32_NO_PWM 99
#define ESP32_DIGITAL (DIGITAL_IN | DIGITAL_OUT)
#define ESP32_ANALOG (DIGITAL_IN | DIGITAL_OUT | ANALOG_IN)
#define ESP32_IN_ANALOG (DIGITAL_IN | ANALOG_IN)

/*
 * PWM capability. The shared resolver works in four abstract PWM channels
 * (slices 0..3), each with an A and a B output that share one frequency;
 * the LEDC backend (hal_pwm_esp32.c) maps slice s -> LEDC timer s with A/B
 * on LEDC channels 2s / 2s+1. A pin advertises its channel via the PWMnX
 * bit in its mode mask, and the .slice field encodes the same channel: low
 * nibble = slice (0..3), bit 0x80 = the B output. Those two encodings must
 * agree (vm_sys_pin reads the mode bit; MM_Misc/checkslice reads .slice).
 *
 * Since LEDC routes any GPIO to any channel, output-capable GPIOs are
 * assigned to slices deterministically by position so that consecutive
 * usable GPIOs form an A/B pair on one slice. Input-only and reserved
 * (console, flash, unbonded) GPIOs keep ESP32_NO_PWM and carry no PWMnX
 * bit.
 */
#define ESP32_PWM_SLICE_B 0x80

#define ESP32_PWM_A(slice) (slice)
#define ESP32_PWM_B(slice) ((slice) | ESP32_PWM_SLICE_B)

#define ESP32_PIN(gpio, modes, adc) \
    {(gpio) + 1, (gpio), "GP" #gpio, (modes), (adc), ESP32_NO_PWM}

/* Output-capable GPIO that can also drive a LEDC PWM channel. `pwmbit` is
 * the PWMnX mode flag and `pwmslice` is the matching .slice byte. */
#define ESP32_PWM_PIN(gpio, modes, adc, pwmbit, pwmslice) \
    {(gpio) + 1, (gpio), "GP" #gpio, (modes) | (pwmbit), (adc), (pwmslice)}

#define ESP32_UNUSED(gpio) \
    {(gpio) + 1, (gpio), "GP" #gpio, UNUSED, ESP32_ADC_NONE, ESP32_NO_PWM}

const uint8_t PINMAP[40] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
    31, 32, 33, 34, 35, 36, 37, 38, 39, 40};

const struct s_PinDef PinDef[NBRPINS + 1] = {
    {0, 99, "NULL", UNUSED, ESP32_ADC_NONE, ESP32_NO_PWM},
    ESP32_PIN(0, DIGITAL_IN, ESP32_ADC_NONE), /* BOOT strap / boot button */
    ESP32_UNUSED(1),                          /* UART0 TX (console) */
    ESP32_PWM_PIN(2, ESP32_DIGITAL, ESP32_ADC_NONE, PWM0A, ESP32_PWM_A(0)),
    ESP32_UNUSED(3), /* UART0 RX (console) */
    ESP32_PWM_PIN(4, ESP32_DIGITAL, ESP32_ADC_NONE, PWM0B, ESP32_PWM_B(0)),
    ESP32_PWM_PIN(5, ESP32_DIGITAL, ESP32_ADC_NONE, PWM1A, ESP32_PWM_A(1)),
    ESP32_UNUSED(6),  /* flash */
    ESP32_UNUSED(7),  /* flash */
    ESP32_UNUSED(8),  /* flash */
    ESP32_UNUSED(9),  /* flash */
    ESP32_UNUSED(10), /* flash */
    ESP32_UNUSED(11), /* flash */
    ESP32_PWM_PIN(12, ESP32_DIGITAL, ESP32_ADC_NONE, PWM1B, ESP32_PWM_B(1)),
    ESP32_PWM_PIN(13, ESP32_DIGITAL, ESP32_ADC_NONE, PWM2A, ESP32_PWM_A(2)),
    ESP32_PWM_PIN(14, ESP32_DIGITAL, ESP32_ADC_NONE, PWM2B, ESP32_PWM_B(2)),
    ESP32_PWM_PIN(15, ESP32_DIGITAL, ESP32_ADC_NONE, PWM3A, ESP32_PWM_A(3)),
    ESP32_PWM_PIN(16, ESP32_DIGITAL, ESP32_ADC_NONE, PWM3B, ESP32_PWM_B(3)),
    ESP32_PWM_PIN(17, ESP32_DIGITAL, ESP32_ADC_NONE, PWM0A, ESP32_PWM_A(0)),
    ESP32_PWM_PIN(18, ESP32_DIGITAL, ESP32_ADC_NONE, PWM0B, ESP32_PWM_B(0)),
    ESP32_PWM_PIN(19, ESP32_DIGITAL, ESP32_ADC_NONE, PWM1A, ESP32_PWM_A(1)),
    ESP32_UNUSED(20), /* not bonded */
    ESP32_PWM_PIN(21, ESP32_DIGITAL | I2C0SDA, ESP32_ADC_NONE, PWM1B, ESP32_PWM_B(1)),
    ESP32_PWM_PIN(22, ESP32_DIGITAL | I2C0SCL, ESP32_ADC_NONE, PWM2A, ESP32_PWM_A(2)),
    ESP32_PWM_PIN(23, ESP32_DIGITAL, ESP32_ADC_NONE, PWM2B, ESP32_PWM_B(2)),
    ESP32_UNUSED(24), /* not bonded */
    ESP32_PWM_PIN(25, ESP32_DIGITAL, ESP32_ADC_NONE, PWM3A, ESP32_PWM_A(3)),
    ESP32_PWM_PIN(26, ESP32_DIGITAL, ESP32_ADC_NONE, PWM3B, ESP32_PWM_B(3)),
    ESP32_PWM_PIN(27, ESP32_DIGITAL, ESP32_ADC_NONE, PWM0A, ESP32_PWM_A(0)),
    ESP32_UNUSED(28), /* not bonded */
    ESP32_UNUSED(29), /* not bonded */
    ESP32_UNUSED(30), /* not bonded */
    ESP32_UNUSED(31), /* not bonded */
    /* ADC1 pins: .adc is the shared channel index hal_pin_esp32.c decodes
     * (0..9 = ADC1 channel). */
    ESP32_PWM_PIN(32, ESP32_ANALOG | I2C1SDA, 4, PWM0B, ESP32_PWM_B(0)),
    ESP32_PWM_PIN(33, ESP32_ANALOG | I2C1SCL, 5, PWM1A, ESP32_PWM_A(1)),
    ESP32_PIN(34, ESP32_IN_ANALOG, 6), /* input-only */
    ESP32_PIN(35, ESP32_IN_ANALOG, 7), /* input-only */
    ESP32_PIN(36, ESP32_IN_ANALOG, 0), /* input-only, SVP */
    ESP32_UNUSED(37),                  /* not bonded */
    ESP32_UNUSED(38),                  /* not bonded */
    ESP32_PIN(39, ESP32_IN_ANALOG, 3), /* input-only, SVN */
};

int codemap(int pin) {
    if (pin < 0 || pin >= 40) error("Invalid GPIO");
    return (int)PINMAP[pin];
}

int IsInvalidPin(int pin) {
    if (pin < 1 || pin > NBRPINS) return true;
    if (PinDef[pin].mode & UNUSED) return true;
    return false;
}

int codecheck(unsigned char * line) {
    if ((line[0] == 'G' || line[0] == 'g') && (line[1] == 'P' || line[1] == 'p')) {
        line += 2;
        if (isnamestart(*line) || *line == '.') return 1;

        if (isdigit(*line) && !isnamechar(line[1])) {
            return 0;
        }
        line++;

        if (!(isdigit(*line))) return 2;
        line++;
        if (isnamechar(*line)) return 3;
    } else
        return 4;
    return 0;
}
