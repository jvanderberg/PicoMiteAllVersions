/*
 * hal_display_backlight_pico.c — RP2040 / RP2350 backlight PWM backend
 * (hal_display_backlight.h).
 *
 * Drives the PWM slice that setpwm() reserved for the LCD backlight
 * (BacklightSlice/BacklightChannel), deriving the clock-divider / wrap /
 * level registers from Option.CPU_Speed.
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"

#include "hardware/pwm.h"

#include "hal/hal_display_backlight.h"

void hal_display_backlight_set(int level, double frequency) {
    int wrap = (Option.CPU_Speed * 1000) / frequency;
    int high = (int)((MMFLOAT)Option.CPU_Speed / frequency * level * 10.0);
    int div = 1;
    while (wrap > 65535) {
        wrap >>= 1;
        if (level >= 0.0) high >>= 1;
        div <<= 1;
    }
    wrap--;
    if (div != 1) pwm_set_clkdiv(BacklightSlice, (float)div);
    pwm_set_wrap(BacklightSlice, wrap);
    pwm_set_chan_level(BacklightSlice, BacklightChannel, high);
}
