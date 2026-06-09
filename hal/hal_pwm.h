/*
 * hal/hal_pwm.h — PWM (and SERVO) output HAL.
 *
 * Channels are abstract ids, not hardware peripheral indices. The shared
 * layer addresses outputs by `channel`; each backend maps that id onto its
 * own hardware (RP keeps its fixed PWM-slice↔pin mapping inside the
 * backend; ESP32 exposes LEDC's channels, any pin onto shared timers).
 * `gpio` is the target's raw GPIO number; translating from MMBasic's
 * 1-based pin numbering is a caller responsibility (PinDef[]), not a HAL
 * concern.
 *
 * SERVO is just PWM: the shared SERVO path calls hal_pwm_configure_pair at
 * 50 Hz with the position mapped to a duty cycle, so there is no separate
 * servo surface here.
 *
 * Global HAL conventions apply (see hal/CONTRACT.md): int returns are 0 on
 * success and negative errno on failure, and the impl never calls
 * MMBasic's error().
 */

#ifndef HAL_PWM_H
#define HAL_PWM_H

#ifdef __cplusplus
extern "C" {
#endif

/* One-time PWM subsystem power-up. Called from board_init.c in the
 * documented init order. Idempotent. Returns 0 on success, negative errno
 * on failure. */
int hal_pwm_init(void);

/* Configure and (unless deferred) start `channel`. A and B are the two
 * outputs the channel pairs; each is configured only when its `has_*`
 * flag is set, driving `gpio_*` at `duty_*` (0–100 %). `frequency` is in
 * Hz and shared by both outputs. `invert` is a polarity bitmask:
 * bit0 inverts A, bit1 inverts B. `phase_correct` selects phase-correct
 * (centre-aligned) counting. When `delaystart` is non-zero the channel is
 * configured but left disabled, to be armed later by hal_pwm_sync_commit.
 * Routes each used pad to the PWM function. Returns 0 on success, or a
 * negative errno on failure (out-of-range channel, unroutable pin, or
 * unreachable frequency). */
int hal_pwm_configure_pair(int channel, float frequency,
                           int has_a, int gpio_a, float duty_a,
                           int has_b, int gpio_b, float duty_b,
                           int invert, int phase_correct, int delaystart);

/* Update the duty cycle of an already-configured `channel` without
 * re-deriving frequency. `which` selects the output: 0 = A, 1 = B.
 * `duty_pct` is 0–100. Returns 0 on success, negative errno on failure. */
int hal_pwm_set_duty(int channel, int which, float duty_pct);

/* PWM SYNC support. hal_pwm_sync_channel stages a counter realignment for
 * one channel: `duty_pct` 0–100 sets the counter to the matching phase,
 * -1.0 leaves the channel's counter alone but still marks it for the
 * synchronised re-enable. Channels that were never started are ignored.
 * hal_pwm_sync_commit atomically re-enables every staged channel so they
 * resume on the same edge. */
void hal_pwm_sync_channel(int channel, float duty_pct);
void hal_pwm_sync_commit(void);

/* Stop `channel`, disabling its output(s) and releasing the pad(s). Safe
 * to call on a channel that is not running (no-op). */
void hal_pwm_stop(int channel);

/* Return the number of PWM channels this backend exposes to the shared
 * layer (the valid range for `channel` is 0 .. value-1). */
int hal_pwm_channels(void);

#ifdef __cplusplus
}
#endif

#endif /* HAL_PWM_H */
