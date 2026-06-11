/*
 * hal_i2c_esp32.c — ESP32-S3 I²C backend (hal_i2c.h).
 *
 * Master role uses the ESP-IDF i2c-master driver (driver/i2c_master.h):
 * one master bus per physical I²C port, with a per-(port,address) device
 * handle added lazily on first use. The shared bus driver
 * (drivers/i2c_bus/I2C.c) parses the BASIC I2C/I2C2 syntax and owns the
 * buffers; this backend maps an abstract bus id onto an IDF port and runs
 * the transaction.
 *
 * Bus mapping: bus 0 -> I2C_NUM_0, bus 1 -> I2C_NUM_1.
 *
 * Single master-bus ownership per port. The new i2c-master driver allows
 * only one master bus per physical port, but two subsystems need master
 * access to I2C_NUM_0: the BASIC system bus (this file) and the Freenove
 * onboard touch/audio bring-up (esp32_freenove_i2c.c). Both obtain the bus
 * through esp32_i2c_master_bus(): the first caller creates it with its
 * pins/pull-up, later callers adopt the cached handle. A reference count
 * tracks the holders so a master_deinit from one subsystem does not pull
 * the bus out from under the other; the bus is deleted only when the last
 * holder releases it. Per-address device handles live in a small cache
 * (esp32_i2c_master_device()) so the same address is not added twice (the
 * driver rejects a duplicate add).
 *
 * Repeated-start (MMBasic "bus hold" / nostop): a no-stop WRITE is buffered
 * and replayed combined with the following READ via
 * i2c_master_transmit_receive, the driver's native repeated-START, which
 * reproduces the register-read pattern (write register pointer,
 * repeated-START, read) that nostop expresses.
 *
 * Slave role lives in hal_i2c_esp32_slave.c, an ESP32-S3 register-level
 * backend; hal_i2c_esp32_internal.h carries the per-port state the two
 * roles share (a port is master XOR slave at any time).
 */

#include <errno.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "driver/i2c_master.h"

#include "hal/hal_i2c.h"
#include "hal_i2c_esp32.h"
#include "hal_i2c_esp32_internal.h"

hal_i2c_esp32_master_port_t hal_i2c_esp32_master_state[HAL_I2C_ESP32_PORTS];
hal_i2c_esp32_bus_t hal_i2c_esp32_bus_state[HAL_I2C_ESP32_PORTS] = {
    {.mux = portMUX_INITIALIZER_UNLOCKED},
    {.mux = portMUX_INITIALIZER_UNLOCKED},
};

int hal_i2c_esp32_port(int bus) {
    if (bus == 0) return I2C_NUM_0;
    if (bus == 1) return I2C_NUM_1;
    return -1;
}

/* ---------------------------------------------------------------------- */
/* Per-port master-bus owner (shared by this file and the Freenove bring-up) */
/* ---------------------------------------------------------------------- */

/* Return the per-port master bus handle, creating it on first use with the
 * supplied SDA/SCL and internal pull-ups. Adds one reference per call; the
 * caller releases with esp32_i2c_master_bus_release(). The pins of an
 * already-created bus are fixed by the first creator; later callers adopt
 * the cached handle regardless of the pins they pass. Returns NULL on a bad
 * port or a creation failure. */
i2c_master_bus_handle_t esp32_i2c_master_bus(int port, int sda_gpio,
                                             int scl_gpio) {
    if (port < 0 || port >= HAL_I2C_ESP32_PORTS) return NULL;
    hal_i2c_esp32_master_port_t * m = &hal_i2c_esp32_master_state[port];
    if (m->bus == NULL) {
        i2c_master_bus_config_t cfg = {
            .i2c_port = port,
            .sda_io_num = (gpio_num_t)sda_gpio,
            .scl_io_num = (gpio_num_t)scl_gpio,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = 1,
        };
        i2c_master_bus_handle_t bus = NULL;
        if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) return NULL;
        m->bus = bus;
        m->sda = sda_gpio;
        m->scl = scl_gpio;
        memset(m->dev, 0, sizeof(m->dev));
    }
    m->refcount++;
    return m->bus;
}

/* Drop one reference on the per-port master bus. When the last reference is
 * released the device cache is torn down and the bus is deleted. */
void esp32_i2c_master_bus_release(int port) {
    if (port < 0 || port >= HAL_I2C_ESP32_PORTS) return;
    hal_i2c_esp32_master_port_t * m = &hal_i2c_esp32_master_state[port];
    if (m->bus == NULL || m->refcount == 0) return;
    if (--m->refcount > 0) return;
    for (int i = 0; i < HAL_I2C_ESP32_MAX_DEVICES; i++) {
        if (m->dev[i].in_use) {
            (void)i2c_master_bus_rm_device(m->dev[i].dev);
            m->dev[i].in_use = 0;
        }
    }
    (void)i2c_del_master_bus(m->bus);
    m->bus = NULL;
}

/* Return a master device handle for `addr` on `port`'s bus at `hz`, adding
 * it on first use. A cached handle whose speed differs is removed and
 * re-added at the new speed. When the cache is full the least-recently-used
 * handle is evicted (an I2C CHECK address scan touches up to 128 addresses,
 * so the cache must recycle); a slot whose handle fails to remove is kept
 * intact and the next-oldest is tried, so a live handle is never overwritten.
 * Returns NULL if the bus is absent, no slot can be freed, or the add
 * fails. */
i2c_master_dev_handle_t esp32_i2c_master_device(int port, uint8_t addr,
                                                uint32_t hz) {
    if (port < 0 || port >= HAL_I2C_ESP32_PORTS) return NULL;
    hal_i2c_esp32_master_port_t * m = &hal_i2c_esp32_master_state[port];
    if (m->bus == NULL) return NULL;
    if (hz == 0) hz = 400000;

    int free_slot = -1;
    for (int i = 0; i < HAL_I2C_ESP32_MAX_DEVICES; i++) {
        if (m->dev[i].in_use && m->dev[i].addr == addr) {
            if (m->dev[i].hz == hz) {
                m->dev[i].last_use = ++m->use_seq;
                return m->dev[i].dev;
            }
            if (i2c_master_bus_rm_device(m->dev[i].dev) != ESP_OK) return NULL;
            m->dev[i].in_use = 0;
            free_slot = i;
            break;
        }
        if (!m->dev[i].in_use && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) {
        int skipped[HAL_I2C_ESP32_MAX_DEVICES] = {0};
        for (int attempt = 0; attempt < HAL_I2C_ESP32_MAX_DEVICES; attempt++) {
            int lru = -1;
            for (int i = 0; i < HAL_I2C_ESP32_MAX_DEVICES; i++) {
                if (!m->dev[i].in_use || skipped[i]) continue;
                if (lru < 0 ||
                    (int32_t)(m->dev[i].last_use - m->dev[lru].last_use) < 0)
                    lru = i;
            }
            if (lru < 0) break;
            if (i2c_master_bus_rm_device(m->dev[lru].dev) != ESP_OK) {
                skipped[lru] = 1;
                continue;
            }
            m->dev[lru].in_use = 0;
            free_slot = lru;
            break;
        }
    }
    if (free_slot < 0) return NULL;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = hz,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(m->bus, &cfg, &dev) != ESP_OK) return NULL;
    m->dev[free_slot].in_use = 1;
    m->dev[free_slot].addr = addr;
    m->dev[free_slot].hz = hz;
    m->dev[free_slot].last_use = ++m->use_seq;
    m->dev[free_slot].dev = dev;
    return dev;
}

int hal_i2c_init(void) {
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Master role                                                            */
/* ---------------------------------------------------------------------- */

int hal_i2c_master_init(int bus, int sda_gpio, int scl_gpio, uint32_t baud) {
    int port = hal_i2c_esp32_port(bus);
    if (port < 0) return -EINVAL;
    hal_i2c_esp32_bus_t * b = &hal_i2c_esp32_bus_state[bus];
    uint32_t hz = baud ? baud : 400000;

    if (b->slave) {
        hal_i2c_slave_disable(bus);
    }
    /* Take one master-bus reference on behalf of the system/user bus. A
     * second open without an intervening deinit keeps a single reference. */
    if (!b->system_holds) {
        if (esp32_i2c_master_bus(port, sda_gpio, scl_gpio) == NULL) return -EIO;
        b->system_holds = 1;
    }
    b->hz = hz;
    b->pending_len = 0;
    return 0;
}

void hal_i2c_master_deinit(int bus) {
    int port = hal_i2c_esp32_port(bus);
    if (port < 0) return;
    hal_i2c_esp32_bus_t * b = &hal_i2c_esp32_bus_state[bus];
    if (!b->system_holds) return;
    b->system_holds = 0;
    b->hz = 0;
    b->pending_len = 0;
    /* Release the system reference; the bus survives while the Freenove
     * touch (or any other holder) still references it. */
    esp32_i2c_master_bus_release(port);
}

int hal_i2c_master_write(int bus, uint8_t addr, const uint8_t * buf, size_t len,
                         int nostop, uint32_t timeout_us) {
    int port = hal_i2c_esp32_port(bus);
    if (port < 0) return -EINVAL;
    hal_i2c_esp32_bus_t * b = &hal_i2c_esp32_bus_state[bus];

    if (nostop) {
        /* Hold the write for the following combined read. */
        if (len > HAL_I2C_ESP32_MAX_PENDING) return -EINVAL;
        memcpy(b->pending, buf, len);
        b->pending_len = (int)len;
        b->pending_addr = addr;
        return (int)len;
    }
    b->pending_len = 0;
    i2c_master_dev_handle_t dev = esp32_i2c_master_device(port, addr, b->hz);
    if (dev == NULL) return -2;
    /* Floor at one FreeRTOS tick: the IDF master converts ms to ticks, so a
       sub-tick timeout collapses to zero and fails the transfer instantly. */
    int tmo_ms = (int)(timeout_us / 1000);
    if (tmo_ms < (int)portTICK_PERIOD_MS) tmo_ms = (int)portTICK_PERIOD_MS;
    esp_err_t err = i2c_master_transmit(dev, buf, len, tmo_ms);
    /* -3 = timeout, -2 = general failure: the status codes the I2C driver
       maps to MM.I2C 2 (timeout) and 1 (NACK). */
    if (err == ESP_ERR_TIMEOUT) return -3;
    if (err != ESP_OK) return -2;
    return (int)len;
}

int hal_i2c_master_read(int bus, uint8_t addr, uint8_t * buf, size_t len,
                        int nostop, uint32_t timeout_us) {
    int port = hal_i2c_esp32_port(bus);
    if (port < 0) return -EINVAL;
    hal_i2c_esp32_bus_t * b = &hal_i2c_esp32_bus_state[bus];
    (void)nostop;
    i2c_master_dev_handle_t dev = esp32_i2c_master_device(port, addr, b->hz);
    if (dev == NULL) return -2;
    /* Floor at one FreeRTOS tick — see hal_i2c_master_write. */
    int tmo_ms = (int)(timeout_us / 1000);
    if (tmo_ms < (int)portTICK_PERIOD_MS) tmo_ms = (int)portTICK_PERIOD_MS;
    esp_err_t err;
    if (b->pending_len > 0 && b->pending_addr == addr) {
        err = i2c_master_transmit_receive(dev, b->pending,
                                          (size_t)b->pending_len, buf, len,
                                          tmo_ms);
        b->pending_len = 0;
    } else {
        b->pending_len = 0;
        err = i2c_master_receive(dev, buf, len, tmo_ms);
    }
    if (err == ESP_ERR_TIMEOUT) return -3;
    if (err != ESP_OK) return -2;
    return (int)len;
}
