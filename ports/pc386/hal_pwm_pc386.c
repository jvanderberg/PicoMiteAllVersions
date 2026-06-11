/*
 * hal_pwm_pc386.c — pc386 PWM backend (hal_pwm.h).
 *
 * The pc386 port drives GPIO over the LPT1 parallel port, which has no
 * PWM generator. The backend advertises zero channels; the shared PWM
 * syscall rejects any PWM/SERVO request as out of bounds before reaching
 * the register entry points, which stay no-ops.
 */

#include "hal/hal_pwm.h"

int hal_pwm_init(void) {
    return 0;
}

int hal_pwm_channels(void) {
    return 0;
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
    return -1;
}

int hal_pwm_set_duty(int channel, int which, float duty_pct) {
    (void)channel;
    (void)which;
    (void)duty_pct;
    return -1;
}

int hal_pwm_query(int channel, uint32_t * top, uint32_t * level_a,
                  uint32_t * level_b) {
    (void)channel;
    (void)top;
    (void)level_a;
    (void)level_b;
    return -1;
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
