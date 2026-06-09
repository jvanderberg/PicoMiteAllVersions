/*
 * shared/peripheral/pwm_cmd.c — BASIC PWM and SERVO command parsers.
 *
 * One copy compiled for every port. Parses the BASIC argument list and
 * dispatches to the shared PWM/SERVO syscall (runtime/vm/vm_sys_pwm.c),
 * which the bytecode VM also calls. All hardware specifics live behind
 * hal_pwm_*; this file touches none of them.
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "vm_sys_pwm.h"

void cmd_pwm(void) {
    unsigned char * tp;
    if ((tp = checkstring(cmdline, (unsigned char *)"SYNC"))) {
        MMFLOAT counts[12];
        uint16_t present = 0;
        int i;
        for (i = 0; i < 12; i++) counts[i] = -1.0;
        getargs(&tp, 23, (unsigned char *)",");
        for (i = 0; i < argc / 2 + 1 && i < 12; i++) {
            if (i * 2 < argc && *argv[i * 2]) {
                counts[i] = getnumber(argv[i * 2]);
                if ((counts[i] < 0.0 || counts[i] > 100.0) && counts[i] != -1.0)
                    error("Syntax");
                present |= (uint16_t)(1u << i);
            }
        }
        vm_sys_pwm_sync(present, counts);
        return;
    }

    getargs(&cmdline, 11, (unsigned char *)",");
    if (argc < 3) error("Syntax");
    {
        int channel = getint(argv[0], 0, 11);
        int phase = 0;
        int defer = 0;
        int has_duty1 = 0, has_duty2 = 0;
        MMFLOAT frequency, duty1 = 0, duty2 = 0;
        if (checkstring(argv[2], (unsigned char *)"OFF")) {
            vm_sys_pwm_off(channel);
            return;
        }
        if (argc < 5) error("Syntax");
        frequency = getnumber(argv[2]);
        if (*argv[4]) {
            duty1 = getnumber(argv[4]);
            has_duty1 = 1;
        }
        if (argc >= 7 && *argv[6]) {
            duty2 = getnumber(argv[6]);
            has_duty2 = 1;
        }
        if (argc >= 9 && *argv[8]) phase = getint(argv[8], 0, 1);
        if (argc == 11 && *argv[10]) defer = getint(argv[10], 0, 1);
        vm_sys_pwm_configure(channel, frequency, has_duty1, duty1, has_duty2, duty2, phase, defer);
    }
}

void cmd_Servo(void) {
    getargs(&cmdline, 5, (unsigned char *)",");
    if (argc < 3) error("Syntax");
    {
        int channel = getint(argv[0], 0, 11);
        int has_pos1 = 0, has_pos2 = 0;
        MMFLOAT pos1 = 0, pos2 = 0;
        if (checkstring(argv[2], (unsigned char *)"OFF")) {
            vm_sys_pwm_off(channel);
            return;
        }
        if (*argv[2]) {
            pos1 = getnumber(argv[2]);
            has_pos1 = 1;
        }
        if (argc >= 5 && *argv[4]) {
            pos2 = getnumber(argv[4]);
            has_pos2 = 1;
        }
        vm_sys_servo_configure(channel, has_pos1, pos1, has_pos2, pos2);
    }
}
