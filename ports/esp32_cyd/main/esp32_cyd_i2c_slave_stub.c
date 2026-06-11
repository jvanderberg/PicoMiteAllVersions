/*
 * esp32_cyd_i2c_slave_stub.c — I²C slave surface for the classic ESP32
 * build (hal_i2c.h slave half).
 *
 * The S3 port's slave backend (hal_i2c_esp32_slave.c) programs the I²C
 * peripheral at register level, and the classic ESP32's register layout
 * differs (no RX/TX FIFO watermark interrupts, different control-register
 * fields). Until a classic-ESP32 slave backend is written, I2C SLAVE OPEN
 * reports a clean failure; the master half (hal_i2c_esp32.c) is fully
 * functional.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "hal/hal_i2c.h"

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
