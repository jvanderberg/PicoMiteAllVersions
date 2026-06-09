/*
 * hal_i2c_host.c — host I²C backend (hal_i2c.h).
 *
 * The host build carries no I²C hardware. Every transaction reports a
 * clean failure (-ENOSYS) so the shared bus driver surfaces the documented
 * "not responding" / MM.I2C error path instead of faking a transfer.
 */

#include <errno.h>

#include "hal/hal_i2c.h"

int hal_i2c_init(void) {
    return 0;
}

int hal_i2c_master_init(int bus, int sda_gpio, int scl_gpio, uint32_t baud) {
    (void)bus;
    (void)sda_gpio;
    (void)scl_gpio;
    (void)baud;
    return -ENOSYS;
}

void hal_i2c_master_deinit(int bus) {
    (void)bus;
}

int hal_i2c_master_write(int bus, uint8_t addr, const uint8_t * buf, size_t len,
                         int nostop, uint32_t timeout_us) {
    (void)bus;
    (void)addr;
    (void)buf;
    (void)len;
    (void)nostop;
    (void)timeout_us;
    return -ENOSYS;
}

int hal_i2c_master_read(int bus, uint8_t addr, uint8_t * buf, size_t len,
                        int nostop, uint32_t timeout_us) {
    (void)bus;
    (void)addr;
    (void)buf;
    (void)len;
    (void)nostop;
    (void)timeout_us;
    return -ENOSYS;
}

int hal_i2c_slave_enable(int bus, uint8_t addr) {
    (void)bus;
    (void)addr;
    return -ENOSYS;
}

int hal_i2c_slave_poll(int bus, uint8_t * buf, size_t cap, size_t * len) {
    (void)bus;
    (void)buf;
    (void)cap;
    if (len) *len = 0;
    return -ENOSYS;
}

int hal_i2c_slave_send(int bus, const uint8_t * buf, size_t len) {
    (void)bus;
    (void)buf;
    (void)len;
    return -ENOSYS;
}

void hal_i2c_slave_disable(int bus) {
    (void)bus;
}
