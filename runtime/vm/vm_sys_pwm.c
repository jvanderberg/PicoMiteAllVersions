/*
 * vm_sys_pwm.c — shared PWM/SERVO syscall + math layer.
 *
 * One copy compiled for every port. Validates BASIC-level PWM/SERVO
 * arguments, resolves each channel's assigned GPIO via the pin module,
 * and drives the hardware through hal_pwm_*. Contains no target hardware
 * access; the per-port backend (hal_pwm_<port>.c) owns the registers.
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "hal/hal_pwm.h"
#include "vm_sys_pwm.h"

/* Slice-claim flags owned by board features (audio voice, LCD backlight,
 * camera clock, keypad backlight) and the rp2350 fast timer. A channel
 * claimed by one of these is off-limits to the BASIC PWM command. These
 * are defined on every port (real on device, sentinel -1 / false on
 * host) so the conflict checks compile and stay dormant where unused. */
extern int BacklightSlice;
extern int CameraSlice;
extern int KeyboardlightSlice;
extern bool fast_timer_active;

static int vm_pwm_invert_from_duty(MMFLOAT * duty) {
    if (*duty < 0.0) {
        *duty = -*duty;
        return 1;
    }
    return 0;
}

void vm_sys_pwm_configure(int channel, MMFLOAT frequency,
                          int has_duty1, MMFLOAT duty1,
                          int has_duty2, MMFLOAT duty2,
                          int phase_correct, int delaystart) {
    int invert = 0;
    int cpu_speed = Option.CPU_Speed;

    if (channel < 0 || channel >= hal_pwm_channels())
        error("Number out of bounds");
    if (channel == 0 && fast_timer_active)
        error("Channel 0 in use for fast timer");
    if (channel == BacklightSlice)
        error("Channel in use for backlight");
    if (channel == Option.AUDIO_SLICE)
        error("Channel in use for Audio");
    if (channel == CameraSlice)
        error("Channel in use for Camera");
    if (channel == KeyboardlightSlice)
        error("Channel in use for keyboard backlight");

    if (frequency <= 0.0)
        error("Invalid frequency");
    /* Phase-correct (center-aligned) PWM counts up then down, halving the
     * output frequency. Double the requested frequency so the channel emits
     * the user-requested Hz. The range check and div/wrap math below all use
     * the doubled value, matching the original interpreter behavior. */
    if (phase_correct)
        frequency *= 2.0;
    if (cpu_speed > 0 && frequency > (MMFLOAT)(cpu_speed >> 2) * 1000.0)
        error("Invalid frequency");
    if (has_duty1 && (duty1 > 100.0 || duty1 < -100.0))
        error("Syntax");
    if (has_duty2 && (duty2 > 100.0 || duty2 < -100.0))
        error("Syntax");
    if (has_duty1 && vm_pwm_invert_from_duty(&duty1)) invert |= 1;
    if (has_duty2 && vm_pwm_invert_from_duty(&duty2)) invert |= 2;

    int pinA = has_duty1 ? vm_pin_pwm_assigned_pin(channel, 0) : 0;
    int pinB = has_duty2 ? vm_pin_pwm_assigned_pin(channel, 1) : 0;
    if (has_duty1 && pinA == 0) error("Pin not set for PWM");
    if (has_duty2 && pinB == 0) error("Pin not set for PWM");

    int gpioA = has_duty1 ? PinDef[pinA].GPno : -1;
    int gpioB = has_duty2 ? PinDef[pinB].GPno : -1;

    int rc = hal_pwm_configure_pair(channel, frequency,
                                    has_duty1, gpioA, has_duty1 ? duty1 : -1.0,
                                    has_duty2, gpioB, has_duty2 ? duty2 : -1.0,
                                    invert, phase_correct, delaystart);
    if (rc != 0) error("Invalid frequency");

    if (has_duty1) vm_pin_pwm_mark_reserved(channel, 0);
    if (has_duty2) vm_pin_pwm_mark_reserved(channel, 1);
}

void vm_sys_pwm_sync(uint16_t present_mask, const MMFLOAT * counts) {
    for (int channel = 0; channel < hal_pwm_channels(); channel++) {
        if (!(present_mask & (1u << channel)))
            continue;
        if ((counts[channel] < 0.0 || counts[channel] > 100.0) && counts[channel] != -1.0)
            error("Syntax");
        hal_pwm_sync_channel(channel, counts[channel]);
    }
    hal_pwm_sync_commit();
}

void vm_sys_pwm_off(int channel) {
    if (channel < 0 || channel >= hal_pwm_channels())
        error("Number out of bounds");
    if (channel == 0 && fast_timer_active)
        error("Channel 0 in use for fast timer");
    if (channel == BacklightSlice)
        error("Channel in use for backlight");
    if (channel == Option.AUDIO_SLICE)
        error("Channel in use for Audio");
    if (channel == CameraSlice)
        error("Channel in use for Camera");
    if (channel == KeyboardlightSlice)
        error("Channel in use for keyboard backlight");
    vm_pin_pwm_release(channel);
    hal_pwm_stop(channel);
}

void vm_sys_servo_configure(int channel,
                            int has_pos1, MMFLOAT pos1,
                            int has_pos2, MMFLOAT pos2) {
    MMFLOAT duty1 = 0, duty2 = 0;
    if (has_pos1) {
        if (pos1 < -20.0 || pos1 > 120.0) error("Syntax");
        duty1 = 5.0 + pos1 * 0.05;
    }
    if (has_pos2) {
        if (pos2 < -20.0 || pos2 > 120.0) error("Syntax");
        duty2 = 5.0 + pos2 * 0.05;
    }
    vm_sys_pwm_configure(channel, 50.0, has_pos1, duty1, has_pos2, duty2, 0, 0);
}
