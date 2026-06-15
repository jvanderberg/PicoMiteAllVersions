/*
 * hal_i2c_esp32_internal.h — state shared between the ESP32 I²C master
 * backend (hal_i2c_esp32.c) and the register-level slave backend
 * (hal_i2c_esp32_slave.c).
 *
 * A given IDF port is either a master or a slave at one time, so the two
 * role TUs coordinate through the same per-port records: the master-bus
 * owner table and the per-bus state (speed, held no-stop write, slave
 * ring/TX staging).
 */

#ifndef HAL_I2C_ESP32_INTERNAL_H
#define HAL_I2C_ESP32_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"

#include "driver/i2c_master.h"
#include "esp_intr_alloc.h"
#include "soc/i2c_struct.h"

#define HAL_I2C_ESP32_PORTS 2

/* Per-(port,addr) master device-handle cache. Eight distinct addresses per
 * bus covers the system devices (RTC, display, keypad) plus the Freenove
 * touch/codec sharing bus 0; less-common addresses (e.g. an I2C CHECK scan
 * over 0..127) recycle slots via LRU eviction. */
#define HAL_I2C_ESP32_MAX_DEVICES 8

/* Held no-stop write awaiting the combined read. */
#define HAL_I2C_ESP32_MAX_PENDING 64

/* Slave buffer sizes. The shared bus driver caps a single
 * I2C SLAVE READ/WRITE at 255 bytes (I2C_Send_Buffer is 256). */
#define HAL_I2C_ESP32_SLAVE_RING 512
#define HAL_I2C_ESP32_SLAVE_TX 256

typedef struct {
    int in_use;
    uint8_t addr;
    uint32_t hz;
    uint32_t last_use;
    i2c_master_dev_handle_t dev;
} hal_i2c_esp32_device_t;

typedef struct {
    i2c_master_bus_handle_t bus;
    int refcount;
    int sda;
    int scl;
    uint32_t use_seq;
    hal_i2c_esp32_device_t dev[HAL_I2C_ESP32_MAX_DEVICES];
} hal_i2c_esp32_master_port_t;

typedef struct {
    int system_holds; /* held by the BASIC system/user bus (this file). */
    uint32_t hz;
    /* Held no-stop write awaiting the combined read. */
    int pending_len;
    uint8_t pending_addr;
    uint8_t pending[HAL_I2C_ESP32_MAX_PENDING];

    /* Slave state. */
    int slave;
    int sda;
    int scl;
    i2c_dev_t * hw;
    intr_handle_t intr;
    volatile uint8_t ring[HAL_I2C_ESP32_SLAVE_RING];
    volatile size_t ring_head;
    volatile size_t ring_tail;
    /* Bytes drained so far in the in-flight receive transaction; the ISR
     * uses it to raise the receive-ready event only for transactions that
     * carried data. */
    volatile size_t rx_trans_len;
    /* Staged response for a master read; the ISR refills the TX FIFO from
     * tx_buf[tx_pos..tx_len) on the TX watermark interrupt. */
    uint8_t tx_buf[HAL_I2C_ESP32_SLAVE_TX];
    volatile size_t tx_len;
    volatile size_t tx_pos;
    portMUX_TYPE mux;
} hal_i2c_esp32_bus_t;

extern hal_i2c_esp32_master_port_t hal_i2c_esp32_master_state[HAL_I2C_ESP32_PORTS];
extern hal_i2c_esp32_bus_t hal_i2c_esp32_bus_state[HAL_I2C_ESP32_PORTS];

/* Map the abstract BASIC bus id onto an IDF port (bus 0 -> I2C_NUM_0,
 * bus 1 -> I2C_NUM_1); -1 for any other id. */
int hal_i2c_esp32_port(int bus);

#endif /* HAL_I2C_ESP32_INTERNAL_H */
