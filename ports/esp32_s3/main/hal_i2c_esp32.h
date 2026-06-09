/*
 * hal_i2c_esp32.h — per-port I²C master-bus owner shared by the BASIC system
 * bus (hal_i2c_esp32.c) and the Freenove onboard touch/audio bring-up
 * (esp32_freenove_i2c.c).
 *
 * The new i2c-master driver allows only one master bus per physical port.
 * Both subsystems obtain the bus through esp32_i2c_master_bus() and their
 * per-address device handles through esp32_i2c_master_device(); a reference
 * count keeps the bus alive until the last holder releases it.
 */

#ifndef HAL_I2C_ESP32_H
#define HAL_I2C_ESP32_H

#include <stdint.h>

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Get (creating on first use) the master bus for IDF `port` with the given
 * SDA/SCL pins and internal pull-ups, taking one reference. The pins of an
 * already-created bus are fixed by its first creator. Returns NULL on a bad
 * port or creation failure. */
i2c_master_bus_handle_t esp32_i2c_master_bus(int port, int sda_gpio,
                                             int scl_gpio);

/* Drop one reference taken by esp32_i2c_master_bus(); deletes the bus and its
 * device cache when the last reference is released. */
void esp32_i2c_master_bus_release(int port);

/* Get (adding on first use) a master device handle for 7-bit `addr` on
 * `port`'s bus at `hz`. A cached entry at a different speed is re-added.
 * Returns NULL if the bus is absent, the cache is full, or the add fails. */
i2c_master_dev_handle_t esp32_i2c_master_device(int port, uint8_t addr,
                                                uint32_t hz);

#ifdef __cplusplus
}
#endif

#endif /* HAL_I2C_ESP32_H */
