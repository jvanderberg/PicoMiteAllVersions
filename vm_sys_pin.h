#ifndef VM_SYS_PIN_H
#define VM_SYS_PIN_H

#include <stdint.h>

enum {
    VM_PIN_MODE_OFF = 0,
    VM_PIN_MODE_DIN = 2,
    VM_PIN_MODE_DOUT = 8
};

void vm_sys_pin_setpin(int64_t pin, int mode);
int64_t vm_sys_pin_read(int64_t pin);
void vm_sys_pin_write(int64_t pin, int64_t value);

#endif
