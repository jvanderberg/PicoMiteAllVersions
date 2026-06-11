/*
 * hal_pwm_pico.c — RP2040 / RP2350 PWM backend (hal_pwm.h).
 *
 * Owns the 12-slice model and the pico-SDK register programming for the
 * BASIC PWM and SERVO commands. Abstract channels map 1:1 onto PWM
 * slices; A/B map onto the slice's two channels. The shared layer
 * (runtime/vm/vm_sys_pwm.c) supplies validated frequency, duty, and GPIO;
 * this file derives the slice clock-divider / wrap / level registers and
 * drives them.
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"

#include "hardware/pwm.h"

#include "hal/hal_pwm.h"

#define HAL_PWM_SLICE_COUNT 12

extern bool rp2350a;

static unsigned char hal_pwm_started[HAL_PWM_SLICE_COUNT];
static uint32_t hal_pwm_sync_enabled;
static int hal_pwm_sync_staged;

static int hal_pwm_max_slice(void) {
    return rp2350a ? 7 : 11;
}

int hal_pwm_init(void) {
    return 0;
}

int hal_pwm_channels(void) {
    return hal_pwm_max_slice() + 1;
}

int hal_pwm_configure_pair(int channel, float frequency,
                           int has_a, int gpio_a, float duty_a,
                           int has_b, int gpio_b, float duty_b,
                           int invert, int phase_correct, int delaystart) {
    int div = 1;
    int high1 = 0, high2 = 0;
    int cpu_speed = Option.CPU_Speed;
    int wrap;

    if (channel < 0 || channel > hal_pwm_max_slice())
        return -1;

    wrap = (cpu_speed * 1000) / frequency;
    if (has_a) high1 = (int)((MMFLOAT)cpu_speed / frequency * duty_a * 10.0);
    if (has_b) high2 = (int)((MMFLOAT)cpu_speed / frequency * duty_b * 10.0);
    while (wrap > 65535) {
        wrap >>= 1;
        if (has_a) high1 >>= 1;
        if (has_b) high2 >>= 1;
        div <<= 1;
    }
    if (div > 256)
        return -1;
    wrap--;
    if (high1) high1--;
    if (high2) high2--;
    pwm_set_clkdiv(channel, (float)div);
    pwm_set_wrap(channel, wrap);
    pwm_set_output_polarity(channel, (invert & 1) ? true : false,
                            (invert & 2) ? true : false);
    pwm_set_phase_correct(channel, phase_correct ? true : false);

    (void)gpio_a;
    (void)gpio_b;
    if (has_a) pwm_set_chan_level(channel, PWM_CHAN_A, high1);
    if (has_b) pwm_set_chan_level(channel, PWM_CHAN_B, high2);
    if (!delaystart) pwm_set_enabled(channel, true);
    hal_pwm_started[channel] = 1;
    return 0;
}

int hal_pwm_set_duty(int channel, int which, float duty_pct) {
    if (channel < 0 || channel > hal_pwm_max_slice())
        return -1;
    uint32_t top = pwm_hw->slice[channel].top;
    int high = (int)((MMFLOAT)(top + 1) * duty_pct / 100.0);
    if (high) high--;
    pwm_set_chan_level(channel, which ? PWM_CHAN_B : PWM_CHAN_A, high);
    return 0;
}

int hal_pwm_query(int channel, uint32_t * top, uint32_t * level_a,
                  uint32_t * level_b) {
    if (channel < 0 || channel > hal_pwm_max_slice())
        return -1;
    if (top) *top = pwm_hw->slice[channel].top;
    if (level_a) *level_a = (pwm_hw->slice[channel].cc & 0xFFFF);
    if (level_b) *level_b = ((pwm_hw->slice[channel].cc) >> 16);
    return 0;
}

void hal_pwm_sync_channel(int channel, float duty_pct) {
    if (channel < 0 || channel > hal_pwm_max_slice())
        return;
    if (!hal_pwm_sync_staged) {
        hal_pwm_sync_enabled = pwm_hw->en;
        hal_pwm_sync_staged = 1;
    }
    if (!hal_pwm_started[channel])
        return;
    pwm_set_enabled(channel, false);
    if (duty_pct >= 0.0) {
        MMFLOAT count = (MMFLOAT)pwm_hw->slice[channel].top * (100.0 - duty_pct) / 100.0;
        pwm_set_counter(channel, (int)count);
    }
    hal_pwm_sync_enabled |= (1u << channel);
}

void hal_pwm_sync_commit(void) {
    if (!hal_pwm_sync_staged)
        return;
    pwm_hw->en = hal_pwm_sync_enabled;
    hal_pwm_sync_staged = 0;
}

void hal_pwm_stop(int channel) {
    if (channel < 0 || channel > hal_pwm_max_slice())
        return;
    pwm_set_enabled(channel, false);
    hal_pwm_started[channel] = 0;
}
