/*
 * hal/hal_i2c.h — I²C master + slave HAL.
 *
 * Buses are abstract ids, not hardware peripheral numbers:
 *   bus 0 = the first I²C controller  (RP: I2C/I2C0; ESP32: I2C_NUM_0)
 *   bus 1 = the second I²C controller (RP: I2C2/I2C1; ESP32: I2C_NUM_1)
 * Each backend maps the abstract bus id onto its own hardware. SDA/SCL are
 * the target's raw GPIO numbers; translating from MMBasic's 1-based pin
 * numbering to the GPIO number is a caller responsibility (PinDef[]), not a
 * HAL concern.
 *
 * Global HAL conventions apply (see hal/CONTRACT.md): int returns are 0 on
 * success and negative errno on failure, the caller owns every buffer, and
 * the impl never calls MMBasic's error().
 */

#ifndef HAL_I2C_H
#define HAL_I2C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-time I²C subsystem power-up. Called from board_init.c in the
 * documented init order. Idempotent; brings up no bus by itself — the
 * BASIC I2C OPEN path drives hal_i2c_master_init for that. Returns 0 on
 * success, negative errno on failure. */
int hal_i2c_init(void);

/* -----------------------------------------------------------------------
 * Master role
 * ---------------------------------------------------------------------- */

/* Bring up `bus` as an I²C master on the given SDA/SCL GPIOs at `baud`
 * Hz. Routes the pads to the I²C function and enables pulls if the
 * backend requires them. Returns 0 on success, negative errno on failure
 * (e.g. -EINVAL for an unknown bus or unroutable pin). */
int hal_i2c_master_init(int bus, int sda_gpio, int scl_gpio, uint32_t baud);

/* Tear `bus` back down, releasing the SDA/SCL pads. Safe to call on a bus
 * that was never initialised (no-op). */
void hal_i2c_master_deinit(int bus);

/* Write `len` bytes from `buf` to the 7-bit slave `addr` on `bus`.
 * `nostop` non-zero suppresses the trailing STOP so a repeated-START
 * read can follow. `timeout_us` bounds the whole transfer.
 * Returns the number of bytes written (>= 0) on success, or a negative
 * errno on failure (e.g. -ETIMEDOUT, -EIO for a NACK). The caller owns
 * `buf`. */
int hal_i2c_master_write(int bus, uint8_t addr, const uint8_t *buf, size_t len,
                         int nostop, uint32_t timeout_us);

/* Read `len` bytes from the 7-bit slave `addr` on `bus` into `buf`.
 * `nostop` non-zero suppresses the trailing STOP. `timeout_us` bounds the
 * whole transfer. Returns the number of bytes read (>= 0) on success, or
 * a negative errno on failure. The caller owns `buf` and guarantees it
 * holds at least `len` bytes. */
int hal_i2c_master_read(int bus, uint8_t addr, uint8_t *buf, size_t len,
                        int nostop, uint32_t timeout_us);

/* -----------------------------------------------------------------------
 * Slave role
 *
 * `bus` is the same abstract id space as the master functions; a given bus
 * is either a master or a slave at one time. The RP backend drives this
 * through its existing register/IRQ slave path; the ESP32 backend uses the
 * ESP-IDF i2c-slave driver.
 * ---------------------------------------------------------------------- */

/* Enable `bus` as an I²C slave listening on the 7-bit `addr`. Returns 0
 * on success, negative errno on failure. */
int hal_i2c_slave_enable(int bus, uint8_t addr);

/* Poll `bus` for bytes received from the master. Up to `cap` bytes are
 * copied into `buf` and the actual count is stored in `*len`. Returns 0
 * on success (including the no-data case, where `*len` is 0), or a
 * negative errno on failure. The caller owns `buf` and `len`. */
int hal_i2c_slave_poll(int bus, uint8_t *buf, size_t cap, size_t *len);

/* Queue `len` bytes from `buf` to send to the master on the next read
 * transaction on `bus`. Returns 0 on success, negative errno on failure.
 * The caller owns `buf`. */
int hal_i2c_slave_send(int bus, const uint8_t *buf, size_t len);

/* Stop acting as a slave on `bus` and release the pads. Safe to call on a
 * bus that is not enabled as a slave (no-op). */
void hal_i2c_slave_disable(int bus);

#ifdef __cplusplus
}
#endif

#endif /* HAL_I2C_H */
