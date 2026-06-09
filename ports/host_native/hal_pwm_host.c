/*
 * hal_pwm_host.c — host PWM backend (hal_pwm.h).
 *
 * The host build has no PWM hardware; it models 12 abstract channels so
 * test programs can exercise the SETPIN/PWM/SERVO command surface. The
 * shared layer performs all BASIC-level validation, so these entry points
 * simply accept the request and report success.
 */

#include "hal/hal_pwm.h"

#define HAL_PWM_HOST_CHANNELS 12

int hal_pwm_init(void) {
    return 0;
}

int hal_pwm_channels(void) {
    return HAL_PWM_HOST_CHANNELS;
}

int hal_pwm_configure_pair(int channel, float frequency,
                           int has_a, int gpio_a, float duty_a,
                           int has_b, int gpio_b, float duty_b,
                           int invert, int phase_correct, int delaystart) {
    (void)channel;
    (void)frequency;
    (void)has_a;
    (void)gpio_a;
    (void)duty_a;
    (void)has_b;
    (void)gpio_b;
    (void)duty_b;
    (void)invert;
    (void)phase_correct;
    (void)delaystart;
    return 0;
}

int hal_pwm_set_duty(int channel, int which, float duty_pct) {
    (void)channel;
    (void)which;
    (void)duty_pct;
    return 0;
}

void hal_pwm_sync_channel(int channel, float duty_pct) {
    (void)channel;
    (void)duty_pct;
}

void hal_pwm_sync_commit(void) {
}

void hal_pwm_stop(int channel) {
    (void)channel;
}
