/*
 * hal_pwm_esp32.c — ESP32-S3 PWM backend (hal_pwm.h) over LEDC.
 *
 * LEDC exposes eight low-speed channels sharing four timers; any GPIO can
 * be routed to any channel. The shared layer (runtime/vm/vm_sys_pwm.c)
 * works in abstract PWM channels that each drive an A and a B output at one
 * frequency. To honour that contract on LEDC, one abstract channel maps to
 * one LEDC timer carrying a pair of LEDC channels: abstract channel s uses
 * timer s, output A on LEDC channel 2s and output B on LEDC channel 2s+1,
 * both clocked by timer s so A and B share a frequency. That yields four
 * abstract channels (0..3) — one per LEDC timer. The shared PWM syscall
 * supplies the validated frequency, duty, and GPIO; this file programs the
 * timer and its two channels.
 */

#include <stdint.h>

#include "driver/ledc.h"
#include "esp_err.h"

#include "hal/hal_pwm.h"

#define HAL_PWM_ESP32_CHANNELS LEDC_TIMER_MAX
#define HAL_PWM_ESP32_DUTY_RES LEDC_TIMER_10_BIT
#define HAL_PWM_ESP32_DUTY_MAX ((1 << 10) - 1)

/* Abstract channel s -> LEDC timer s; outputs A/B -> LEDC channels 2s / 2s+1. */
static int esp32_pwm_ledc_channel(int channel, int which) {
    return channel * 2 + (which ? 1 : 0);
}

int hal_pwm_init(void) {
    return 0;
}

int hal_pwm_channels(void) {
    return HAL_PWM_ESP32_CHANNELS;
}

static int esp32_pwm_timer_config(int channel, float frequency) {
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = HAL_PWM_ESP32_DUTY_RES,
        .timer_num = (ledc_timer_t)channel,
        .freq_hz = (uint32_t)frequency,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    return ledc_timer_config(&timer) == ESP_OK ? 0 : -1;
}

static int esp32_pwm_start(int channel, int which, int gpio, float duty_pct,
                           int invert) {
    if (gpio < 0) return -1;

    uint32_t duty = (uint32_t)(duty_pct * HAL_PWM_ESP32_DUTY_MAX / 100.0f + 0.5f);
    if (duty > HAL_PWM_ESP32_DUTY_MAX) duty = HAL_PWM_ESP32_DUTY_MAX;

    ledc_channel_config_t cfg = {
        .gpio_num = gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)esp32_pwm_ledc_channel(channel, which),
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = (ledc_timer_t)channel,
        .duty = duty,
        .hpoint = 0,
        .flags.output_invert = invert ? 1 : 0,
    };
    if (ledc_channel_config(&cfg) != ESP_OK) return -1;
    return 0;
}

int hal_pwm_configure_pair(int channel, float frequency,
                           int has_a, int gpio_a, float duty_a,
                           int has_b, int gpio_b, float duty_b,
                           int invert, int phase_correct, int delaystart) {
    (void)phase_correct;
    (void)delaystart;
    if (channel < 0 || channel >= HAL_PWM_ESP32_CHANNELS) return -1;
    if (frequency <= 0.0f) return -1;
    if (!has_a && !has_b) return -1;

    /* A and B share the abstract channel's timer, so its frequency is set
     * once for the pair. */
    if (esp32_pwm_timer_config(channel, frequency) != 0) return -1;

    if (has_a) {
        int rc = esp32_pwm_start(channel, 0, gpio_a, duty_a, invert & 1);
        if (rc != 0) return rc;
    }
    if (has_b) {
        int rc = esp32_pwm_start(channel, 1, gpio_b, duty_b, invert & 2);
        if (rc != 0) return rc;
    }
    return 0;
}

int hal_pwm_set_duty(int channel, int which, float duty_pct) {
    if (channel < 0 || channel >= HAL_PWM_ESP32_CHANNELS) return -1;
    ledc_channel_t target = (ledc_channel_t)esp32_pwm_ledc_channel(channel, which);
    uint32_t duty = (uint32_t)(duty_pct * HAL_PWM_ESP32_DUTY_MAX / 100.0f + 0.5f);
    if (duty > HAL_PWM_ESP32_DUTY_MAX) duty = HAL_PWM_ESP32_DUTY_MAX;
    if (ledc_set_duty(LEDC_LOW_SPEED_MODE, target, duty) != ESP_OK)
        return -1;
    if (ledc_update_duty(LEDC_LOW_SPEED_MODE, target) != ESP_OK)
        return -1;
    return 0;
}

void hal_pwm_sync_channel(int channel, float duty_pct) {
    if (channel < 0 || channel >= HAL_PWM_ESP32_CHANNELS) return;
    if (duty_pct < 0.0f) return;
    hal_pwm_set_duty(channel, 0, duty_pct);
}

void hal_pwm_sync_commit(void) {
}

void hal_pwm_stop(int channel) {
    if (channel < 0 || channel >= HAL_PWM_ESP32_CHANNELS) return;
    ledc_stop(LEDC_LOW_SPEED_MODE,
              (ledc_channel_t)esp32_pwm_ledc_channel(channel, 0), 0);
    ledc_stop(LEDC_LOW_SPEED_MODE,
              (ledc_channel_t)esp32_pwm_ledc_channel(channel, 1), 0);
}
