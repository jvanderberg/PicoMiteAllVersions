#ifndef VM_SYS_PWM_H
#define VM_SYS_PWM_H

#include <stdint.h>
#include "configuration.h"

/* Shared PWM/SERVO syscall surface. Both execution engines funnel here:
 * the interpreter (cmd_pwm / cmd_Servo) and the bytecode VM (OP_SYSCALL).
 * The bodies validate BASIC-level arguments, resolve each PWM channel to
 * its assigned GPIO, and drive the hardware through hal_pwm_*. No target
 * hardware is touched directly. */

void vm_sys_pwm_configure(int channel, MMFLOAT frequency,
                          int has_duty1, MMFLOAT duty1,
                          int has_duty2, MMFLOAT duty2,
                          int phase_correct, int delaystart);
void vm_sys_pwm_sync(uint16_t present_mask, const MMFLOAT * counts);
void vm_sys_pwm_off(int channel);
void vm_sys_servo_configure(int channel,
                            int has_pos1, MMFLOAT pos1,
                            int has_pos2, MMFLOAT pos2);

/* Pin module accessors. SETPIN assigns a BASIC pin index to a PWM
 * channel output (A=0, B=1); the shared syscall reads that assignment to
 * obtain the GPIO it hands to hal_pwm_configure. Returns the pin index or
 * 0 when nothing is assigned. Defined by each port's pin syscall TU. */
int vm_pin_pwm_assigned_pin(int channel, int which);
/* Mark a configured channel output as reserved/started, or clear it on
 * OFF. The pin module owns the ExtCurrentConfig bookkeeping for the
 * assigned pads. which: 0=A, 1=B. */
void vm_pin_pwm_mark_reserved(int channel, int which);
void vm_pin_pwm_release(int channel);
/* Bind a BASIC pin to a PWM channel output from the interpreter SETPIN
 * path. `mode` is a VM_PIN_MODE_PWM* code (== the EXT_PWM* code). On ports
 * whose SETPIN does not maintain a channel↔pin map this is a no-op. */
void vm_pin_pwm_register_setpin(int pin, int mode);

#endif /* VM_SYS_PWM_H */
