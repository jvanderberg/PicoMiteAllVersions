/*
 * hal/hal_display_backlight.h — PWM-driven LCD backlight brightness.
 *
 * Covers the PWM arm of the backlight path only: panels whose backlight
 * pin was claimed at OPTION LCDPANEL time (setpwm() reserves the slice
 * and records it in BacklightSlice/BacklightChannel on RP targets). The
 * other backlight mechanisms — PicoCalc keypad-MCU I2C, SSD1963
 * register, I2C-OLED contrast — have their own hooks and do not route
 * here. The RP backend (ports/pico_sdk_common/hal_display_backlight_pico.c)
 * owns the clock-divider / wrap / level math for the reserved slice.
 *
 * Only device builds compile the callers (External.c setBacklight and
 * MM_Misc.c OPTION LCDPANEL CONSOLE), so non-Pico ports need no stub.
 */

#ifndef HAL_DISPLAY_BACKLIGHT_H
#define HAL_DISPLAY_BACKLIGHT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Reprogram the backlight PWM to `level` percent (0-100) brightness at
 * `frequency` Hz. The caller resolves the frequency policy (explicit
 * BACKLIGHT argument or per-display default) before calling. */
void hal_display_backlight_set(int level, double frequency);

#ifdef __cplusplus
}
#endif

#endif /* HAL_DISPLAY_BACKLIGHT_H */
