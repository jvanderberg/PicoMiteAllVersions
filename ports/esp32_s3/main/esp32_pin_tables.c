/*
 * ESP32-S3 GPIO map.
 *
 * BASIC's GPn syntax maps through codemap() into PinDef[] slots. Keep slot
 * zero as the legacy NULL row, then map GP0..GP48 to slots 1..49 so shared
 * pin-state arrays keep their usual 1-based indexing.
 */

#include <ctype.h>
#include <stdint.h>

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"

#define ESP32_ADC_NONE 99
#define ESP32_NO_PWM 99
#define ESP32_DIGITAL (DIGITAL_IN | DIGITAL_OUT)
#define ESP32_ANALOG (DIGITAL_IN | DIGITAL_OUT | ANALOG_IN)

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
 * GPIOs form an A/B pair on one slice. ESP32_PWM_A/ESP32_PWM_B supply the
 * matching (mode bit, slice byte). Input-only and reserved (USB, flash,
 * unconnected) GPIOs keep ESP32_NO_PWM and carry no PWMnX bit.
 */
#define ESP32_PWM_SLICE_B 0x80

#define ESP32_PWM_A(slice) (slice)
#define ESP32_PWM_B(slice) ((slice) | ESP32_PWM_SLICE_B)

#define ESP32_PWM0A PWM0A
#define ESP32_PWM0B PWM0B
#define ESP32_PWM1A PWM1A
#define ESP32_PWM1B PWM1B
#define ESP32_PWM2A PWM2A
#define ESP32_PWM2B PWM2B
#define ESP32_PWM3A PWM3A
#define ESP32_PWM3B PWM3B

#define ESP32_PIN(gpio, modes, adc) \
    {(gpio) + 1, (gpio), "GP" #gpio, (modes), (adc), ESP32_NO_PWM}

/* Output-capable GPIO that can also drive a LEDC PWM channel. `pwmbit` is
 * the PWMnX mode flag and `pwmslice` is the matching .slice byte. */
#define ESP32_PWM_PIN(gpio, modes, adc, pwmbit, pwmslice) \
    {(gpio) + 1, (gpio), "GP" #gpio, (modes) | (pwmbit), (adc), (pwmslice)}

#define ESP32_UNUSED(gpio) \
    {(gpio) + 1, (gpio), "GP" #gpio, UNUSED, ESP32_ADC_NONE, ESP32_NO_PWM}

const uint8_t PINMAP[49] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
    31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49};

const struct s_PinDef PinDef[NBRPINS + 1] = {
    {0, 99, "NULL", UNUSED, ESP32_ADC_NONE, ESP32_NO_PWM},
    ESP32_PIN(0, DIGITAL_IN, ESP32_ADC_NONE), /* BOOT switch, input-only here */
    ESP32_PWM_PIN(1, ESP32_ANALOG, 0, ESP32_PWM0A, ESP32_PWM_A(0)),
    ESP32_PWM_PIN(2, ESP32_ANALOG, 1, ESP32_PWM0B, ESP32_PWM_B(0)),
    ESP32_PWM_PIN(3, ESP32_ANALOG, 2, ESP32_PWM1A, ESP32_PWM_A(1)),
    ESP32_PWM_PIN(4, ESP32_ANALOG | I2C1SDA, 3, ESP32_PWM1B, ESP32_PWM_B(1)),
    ESP32_PWM_PIN(5, ESP32_ANALOG | I2C1SCL, 4, ESP32_PWM2A, ESP32_PWM_A(2)),
    ESP32_PWM_PIN(6, ESP32_ANALOG, 5, ESP32_PWM2B, ESP32_PWM_B(2)),
    ESP32_PWM_PIN(7, ESP32_ANALOG, 6, ESP32_PWM3A, ESP32_PWM_A(3)),
    ESP32_PWM_PIN(8, ESP32_ANALOG, 7, ESP32_PWM3B, ESP32_PWM_B(3)),
    ESP32_PWM_PIN(9, ESP32_ANALOG, 8, ESP32_PWM0A, ESP32_PWM_A(0)),
    ESP32_PWM_PIN(10, ESP32_ANALOG, 9, ESP32_PWM0B, ESP32_PWM_B(0)),
    ESP32_PWM_PIN(11, ESP32_ANALOG, 10, ESP32_PWM1A, ESP32_PWM_A(1)),
    ESP32_PWM_PIN(12, ESP32_ANALOG, 11, ESP32_PWM1B, ESP32_PWM_B(1)),
    ESP32_PWM_PIN(13, ESP32_ANALOG, 12, ESP32_PWM2A, ESP32_PWM_A(2)), /* onboard LED */
    ESP32_PWM_PIN(14, ESP32_ANALOG | I2C1SDA | I2C1SCL, 13, ESP32_PWM2B, ESP32_PWM_B(2)),
    ESP32_PWM_PIN(15, ESP32_ANALOG | I2C0SCL, 14, ESP32_PWM3A, ESP32_PWM_A(3)),
    ESP32_PWM_PIN(16, ESP32_ANALOG | I2C0SDA, 15, ESP32_PWM3B, ESP32_PWM_B(3)),
    ESP32_PWM_PIN(17, ESP32_ANALOG, 16, ESP32_PWM0A, ESP32_PWM_A(0)),
    ESP32_PWM_PIN(18, ESP32_ANALOG, 17, ESP32_PWM0B, ESP32_PWM_B(0)),
    ESP32_UNUSED(19), /* native USB D- */
    ESP32_UNUSED(20), /* native USB D+ */
    ESP32_PWM_PIN(21, ESP32_DIGITAL | I2C1SDA | I2C1SCL, ESP32_ADC_NONE, ESP32_PWM1A, ESP32_PWM_A(1)),
    ESP32_UNUSED(22),
    ESP32_UNUSED(23),
    ESP32_UNUSED(24),
    ESP32_UNUSED(25),
    ESP32_UNUSED(26),
    ESP32_UNUSED(27),
    ESP32_UNUSED(28),
    ESP32_UNUSED(29),
    ESP32_UNUSED(30),
    ESP32_UNUSED(31),
    ESP32_UNUSED(32),
    ESP32_UNUSED(33),
    ESP32_UNUSED(34),
    ESP32_UNUSED(35),
    ESP32_UNUSED(36),
    ESP32_UNUSED(37),
    ESP32_PWM_PIN(38, ESP32_DIGITAL, ESP32_ADC_NONE, ESP32_PWM1B, ESP32_PWM_B(1)),
    ESP32_PWM_PIN(39, ESP32_DIGITAL, ESP32_ADC_NONE, ESP32_PWM2A, ESP32_PWM_A(2)),
    ESP32_PWM_PIN(40, ESP32_DIGITAL, ESP32_ADC_NONE, ESP32_PWM2B, ESP32_PWM_B(2)),
    ESP32_PWM_PIN(41, ESP32_DIGITAL, ESP32_ADC_NONE, ESP32_PWM3A, ESP32_PWM_A(3)),
    ESP32_PWM_PIN(42, ESP32_DIGITAL | I2C1SCL, ESP32_ADC_NONE, ESP32_PWM3B, ESP32_PWM_B(3)),
    ESP32_PWM_PIN(43, ESP32_DIGITAL, ESP32_ADC_NONE, ESP32_PWM0A, ESP32_PWM_A(0)),
    ESP32_PWM_PIN(44, ESP32_DIGITAL, ESP32_ADC_NONE, ESP32_PWM0B, ESP32_PWM_B(0)),
    ESP32_PWM_PIN(45, ESP32_DIGITAL, ESP32_ADC_NONE, ESP32_PWM1A, ESP32_PWM_A(1)),
    ESP32_PWM_PIN(46, ESP32_DIGITAL, ESP32_ADC_NONE, ESP32_PWM1B, ESP32_PWM_B(1)),
    ESP32_PWM_PIN(47, ESP32_DIGITAL, ESP32_ADC_NONE, ESP32_PWM2A, ESP32_PWM_A(2)),
    ESP32_PWM_PIN(48, ESP32_DIGITAL, ESP32_ADC_NONE, ESP32_PWM2B, ESP32_PWM_B(2)),
};

int codemap(int pin) {
    if (pin < 0 || pin >= 49) error("Invalid GPIO");
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
