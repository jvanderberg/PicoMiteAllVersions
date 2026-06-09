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
 * Slave role uses the ESP-IDF i2c-slave driver (driver/i2c_slave.h). A
 * given IDF port is either a master or a slave at one time; enabling slave
 * mode first tears down the master bus on that port (so I2C SLAVE OPEN on
 * bus 0 is incompatible with the Freenove touch, which holds the bus-0
 * master). The slave SDA/SCL GPIOs come from PinDef[] via the
 * I2C0SDApin / I2C1SDApin assignments that the shared bus driver configures
 * with ExtCfg() before it calls hal_i2c_slave_enable.
 *
 * Slave receive interrupt: on_recv_done fires in ISR context on transaction
 * STOP. It copies the received bytes into a per-bus ring and raises the same
 * status bit (I2C_Status_Slave_Receive_Rdy on I2C_Status for bus 0,
 * I2C2_Status for bus 1) that the MMBasic interrupt dispatcher watches, so a
 * BASIC `I2C SLAVE OPEN`'s receive interrupt subroutine fires, then re-arms
 * i2c_slave_receive for the next transaction. hal_i2c_slave_poll drains the
 * ring; hal_i2c_slave_send pre-loads the response via i2c_slave_transmit.
 *
 * ESP32-S3 send-side limit: the S3 lacks SOC_I2C_SLAVE_CAN_GET_STRETCH_CAUSE,
 * so the slave driver exposes no master-read-request (clock-stretch) ISR —
 * there is no on_stretch_occur member to register and the BASIC slave *send*
 * interrupt subroutine cannot fire on this silicon. Response data is
 * pre-loaded with i2c_slave_transmit and clocked out when the master reads.
 *
 * IDF 5.3 length limitation: in FIFO-mode slave receive the on_recv_done event
 * (i2c_slave_rx_done_event_data_t) carries only the receive buffer pointer, not
 * the number of bytes the master wrote, and the driver-private received length
 * is not exposed. The length is therefore recovered by pre-filling the staging
 * buffer with a 0xFF sentinel before each arm and taking it as the position
 * past the last byte that differs from the sentinel. A message whose final wire
 * byte equals the 0xFF sentinel is truncated by one byte: that trailing 0xFF is
 * trimmed and dropped from the ring, so both the reported length and that data
 * byte are lost. This is a known limitation of IDF 5.3 FIFO-mode slave receive,
 * which reports no length to on_recv_done.
 */

#include <errno.h>
#include <string.h>

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2c_slave.h"

#include "hal/hal_i2c.h"
#include "hal_i2c_esp32.h"
#include "i2c_config.h"

#define HAL_I2C_ESP32_PORTS 2

/* Per-(port,addr) master device-handle cache. Eight distinct addresses per
 * bus covers the system devices (RTC, display, keypad) plus the Freenove
 * touch/codec sharing bus 0. */
#define HAL_I2C_ESP32_MAX_DEVICES 8

/* Held no-stop write awaiting the combined read. */
#define HAL_I2C_ESP32_MAX_PENDING 64

/* Slave staging/ring sizes. The shared bus driver caps a single
 * I2C SLAVE READ/WRITE at 255 bytes (I2C_Send_Buffer is 256). */
#define HAL_I2C_ESP32_SLAVE_STAGING 256
#define HAL_I2C_ESP32_SLAVE_RING 512
#define HAL_I2C_ESP32_SLAVE_SENTINEL 0xFF

typedef struct {
    int in_use;
    uint8_t addr;
    uint32_t hz;
    i2c_master_dev_handle_t dev;
} hal_i2c_esp32_device_t;

typedef struct {
    i2c_master_bus_handle_t bus;
    int refcount;
    int sda;
    int scl;
    hal_i2c_esp32_device_t dev[HAL_I2C_ESP32_MAX_DEVICES];
} hal_i2c_esp32_master_port_t;

typedef struct {
    int system_holds;  /* held by the BASIC system/user bus (this file). */
    uint32_t hz;
    /* Held no-stop write awaiting the combined read. */
    int pending_len;
    uint8_t pending_addr;
    uint8_t pending[HAL_I2C_ESP32_MAX_PENDING];

    /* Slave state. */
    int slave;
    int sda;
    int scl;
    i2c_slave_dev_handle_t slave_dev;
    uint8_t staging[HAL_I2C_ESP32_SLAVE_STAGING];
    volatile uint8_t ring[HAL_I2C_ESP32_SLAVE_RING];
    volatile size_t ring_head;
    volatile size_t ring_tail;
    portMUX_TYPE ring_mux;
    /* on_recv_done runs in ISR context and cannot re-arm i2c_slave_receive
     * (it would block on the driver's rx mutex), so it signals this
     * semaphore and a worker task drains the staging buffer and re-arms. */
    SemaphoreHandle_t rx_signal;
    TaskHandle_t rx_task;
    volatile int rx_run;
} hal_i2c_esp32_bus_t;

static hal_i2c_esp32_master_port_t s_master[HAL_I2C_ESP32_PORTS];
static hal_i2c_esp32_bus_t s_bus[HAL_I2C_ESP32_PORTS] = {
    {.ring_mux = portMUX_INITIALIZER_UNLOCKED},
    {.ring_mux = portMUX_INITIALIZER_UNLOCKED},
};

static int hal_i2c_port(int bus) {
    if (bus == 0) return I2C_NUM_0;
    if (bus == 1) return I2C_NUM_1;
    return -1;
}

/* Resolve the slave bus's SDA/SCL GPIO numbers from the shared pin
 * assignments. i2cSlave()/i2c2Slave() set I2C0SDApin / I2C1SDApin and call
 * ExtCfg() on them before hal_i2c_slave_enable, so PinDef[] holds the GPIO. */
static int hal_i2c_slave_pins(int bus, int *sda_gpio, int *scl_gpio) {
    uint8_t sda_pin, scl_pin;
    if (bus == 0) {
        sda_pin = I2C0SDApin;
        scl_pin = I2C0SCLpin;
    } else if (bus == 1) {
        sda_pin = I2C1SDApin;
        scl_pin = I2C1SCLpin;
    } else {
        return -1;
    }
    if (sda_pin == 99 || scl_pin == 99) return -1;
    *sda_gpio = PinDef[sda_pin].GPno;
    *scl_gpio = PinDef[scl_pin].GPno;
    return 0;
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
    hal_i2c_esp32_master_port_t *m = &s_master[port];
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
    hal_i2c_esp32_master_port_t *m = &s_master[port];
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
 * re-added at the new speed. Returns NULL if the bus is absent, the cache is
 * full, or the add fails. */
i2c_master_dev_handle_t esp32_i2c_master_device(int port, uint8_t addr,
                                                uint32_t hz) {
    if (port < 0 || port >= HAL_I2C_ESP32_PORTS) return NULL;
    hal_i2c_esp32_master_port_t *m = &s_master[port];
    if (m->bus == NULL) return NULL;
    if (hz == 0) hz = 400000;

    int free_slot = -1;
    for (int i = 0; i < HAL_I2C_ESP32_MAX_DEVICES; i++) {
        if (m->dev[i].in_use && m->dev[i].addr == addr) {
            if (m->dev[i].hz == hz) return m->dev[i].dev;
            (void)i2c_master_bus_rm_device(m->dev[i].dev);
            m->dev[i].in_use = 0;
            free_slot = i;
            break;
        }
        if (!m->dev[i].in_use && free_slot < 0) free_slot = i;
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
    int port = hal_i2c_port(bus);
    if (port < 0) return -EINVAL;
    hal_i2c_esp32_bus_t *b = &s_bus[bus];
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
    int port = hal_i2c_port(bus);
    if (port < 0) return;
    hal_i2c_esp32_bus_t *b = &s_bus[bus];
    if (!b->system_holds) return;
    b->system_holds = 0;
    b->hz = 0;
    b->pending_len = 0;
    /* Release the system reference; the bus survives while the Freenove
     * touch (or any other holder) still references it. */
    esp32_i2c_master_bus_release(port);
}

int hal_i2c_master_write(int bus, uint8_t addr, const uint8_t *buf, size_t len,
                         int nostop, uint32_t timeout_us) {
    int port = hal_i2c_port(bus);
    if (port < 0) return -EINVAL;
    hal_i2c_esp32_bus_t *b = &s_bus[bus];

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
    int tmo_ms = (int)((timeout_us / 1000) ? (timeout_us / 1000) : 1);
    esp_err_t err = i2c_master_transmit(dev, buf, len, tmo_ms);
    /* -3 = timeout, -2 = general failure: the status codes the I2C driver
       maps to MM.I2C 2 (timeout) and 1 (NACK). */
    if (err == ESP_ERR_TIMEOUT) return -3;
    if (err != ESP_OK) return -2;
    return (int)len;
}

int hal_i2c_master_read(int bus, uint8_t addr, uint8_t *buf, size_t len,
                        int nostop, uint32_t timeout_us) {
    int port = hal_i2c_port(bus);
    if (port < 0) return -EINVAL;
    hal_i2c_esp32_bus_t *b = &s_bus[bus];
    (void)nostop;
    i2c_master_dev_handle_t dev = esp32_i2c_master_device(port, addr, b->hz);
    if (dev == NULL) return -2;
    int tmo_ms = (int)((timeout_us / 1000) ? (timeout_us / 1000) : 1);
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

/* ---------------------------------------------------------------------- */
/* Slave role                                                             */
/* ---------------------------------------------------------------------- */

static void hal_i2c_slave_ring_push(hal_i2c_esp32_bus_t *b,
                                    const uint8_t *data, size_t len) {
    portENTER_CRITICAL(&b->ring_mux);
    for (size_t i = 0; i < len; i++) {
        size_t next = (b->ring_head + 1) % HAL_I2C_ESP32_SLAVE_RING;
        if (next == b->ring_tail) break;  /* full: drop the overflow. */
        b->ring[b->ring_head] = data[i];
        b->ring_head = next;
    }
    portEXIT_CRITICAL(&b->ring_mux);
}

/* ISR-context receive-done callback. It cannot re-arm i2c_slave_receive (that
 * blocks on the driver's rx mutex), so it only wakes the worker task. The
 * staging buffer it filled stays intact until re-armed. */
static bool IRAM_ATTR hal_i2c_slave_on_recv(i2c_slave_dev_handle_t dev,
                                            const i2c_slave_rx_done_event_data_t *evt,
                                            void *arg) {
    (void)dev;
    (void)evt;
    int bus = (int)(intptr_t)arg;
    hal_i2c_esp32_bus_t *b = &s_bus[bus];
    BaseType_t woken = pdFALSE;
    if (b->rx_signal) xSemaphoreGiveFromISR(b->rx_signal, &woken);
    return woken == pdTRUE;
}

/* Worker: drains the staging buffer the ISR filled into the ring, raises the
 * MMBasic receive-ready status bit, and re-arms the next receive. */
static void hal_i2c_slave_rx_worker(void *arg) {
    int bus = (int)(intptr_t)arg;
    hal_i2c_esp32_bus_t *b = &s_bus[bus];
    while (b->rx_run) {
        if (xSemaphoreTake(b->rx_signal, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        if (!b->rx_run) break;

        /* IDF 5.3 reports no length; recover it from the sentinel pre-fill. */
        size_t n = HAL_I2C_ESP32_SLAVE_STAGING;
        while (n > 0 && b->staging[n - 1] == HAL_I2C_ESP32_SLAVE_SENTINEL) n--;
        if (n > 0) {
            hal_i2c_slave_ring_push(b, b->staging, n);
            if (bus == 0)
                I2C_Status |= I2C_Status_Slave_Receive_Rdy;
            else
                I2C2_Status |= I2C_Status_Slave_Receive_Rdy;
        }

        /* Re-arm for the next transaction with a fresh sentinel fill. */
        if (b->slave_dev) {
            memset(b->staging, HAL_I2C_ESP32_SLAVE_SENTINEL, sizeof(b->staging));
            (void)i2c_slave_receive(b->slave_dev, b->staging,
                                    sizeof(b->staging));
        }
    }
    b->rx_task = NULL;
    vTaskDelete(NULL);
}

int hal_i2c_slave_enable(int bus, uint8_t addr) {
    int port = hal_i2c_port(bus);
    if (port < 0) return -EINVAL;
    hal_i2c_esp32_bus_t *b = &s_bus[bus];

    int sda_gpio = -1, scl_gpio = -1;
    if (hal_i2c_slave_pins(bus, &sda_gpio, &scl_gpio) != 0) return -EINVAL;

    /* A port is master XOR slave. Drop the system master reference (and, if
     * it was the last one, the bus itself) before bringing up the slave. */
    if (b->system_holds) {
        b->system_holds = 0;
        esp32_i2c_master_bus_release(port);
    }
    if (b->slave) {
        hal_i2c_slave_disable(bus);
    }

    i2c_slave_config_t cfg = {
        .i2c_port = port,
        .sda_io_num = (gpio_num_t)sda_gpio,
        .scl_io_num = (gpio_num_t)scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .send_buf_depth = HAL_I2C_ESP32_SLAVE_RING,
        .slave_addr = addr,
        .addr_bit_len = I2C_ADDR_BIT_LEN_7,
    };
    i2c_slave_dev_handle_t dev = NULL;
    if (i2c_new_slave_device(&cfg, &dev) != ESP_OK) return -EIO;

    i2c_slave_event_callbacks_t cbs = {
        .on_recv_done = hal_i2c_slave_on_recv,
    };
    if (i2c_slave_register_event_callbacks(dev, &cbs, (void *)(intptr_t)bus) !=
        ESP_OK) {
        (void)i2c_del_slave_device(dev);
        return -EIO;
    }

    b->slave_dev = dev;
    b->slave = 1;
    b->sda = sda_gpio;
    b->scl = scl_gpio;
    b->ring_head = 0;
    b->ring_tail = 0;
    b->pending_len = 0;

    if (b->rx_signal == NULL) {
        b->rx_signal = xSemaphoreCreateBinary();
        if (b->rx_signal == NULL) {
            (void)i2c_del_slave_device(dev);
            b->slave = 0;
            b->slave_dev = NULL;
            return -ENOMEM;
        }
    }
    b->rx_run = 1;
    if (b->rx_task == NULL) {
        if (xTaskCreate(hal_i2c_slave_rx_worker, "i2c_slv_rx", 3072,
                        (void *)(intptr_t)bus, 10, &b->rx_task) != pdPASS) {
            b->rx_run = 0;
            (void)i2c_del_slave_device(dev);
            b->slave = 0;
            b->slave_dev = NULL;
            return -EIO;
        }
    }

    memset(b->staging, HAL_I2C_ESP32_SLAVE_SENTINEL, sizeof(b->staging));
    if (i2c_slave_receive(dev, b->staging, sizeof(b->staging)) != ESP_OK) {
        b->rx_run = 0;
        if (b->rx_signal) xSemaphoreGive(b->rx_signal);
        (void)i2c_del_slave_device(dev);
        b->slave = 0;
        b->slave_dev = NULL;
        return -EIO;
    }
    return 0;
}

int hal_i2c_slave_poll(int bus, uint8_t *buf, size_t cap, size_t *len) {
    if (len) *len = 0;
    int port = hal_i2c_port(bus);
    if (port < 0) return -EINVAL;
    hal_i2c_esp32_bus_t *b = &s_bus[bus];
    if (!b->slave) return -EIO;

    size_t got = 0;
    portENTER_CRITICAL(&b->ring_mux);
    while (got < cap && b->ring_tail != b->ring_head) {
        buf[got++] = b->ring[b->ring_tail];
        b->ring_tail = (b->ring_tail + 1) % HAL_I2C_ESP32_SLAVE_RING;
    }
    portEXIT_CRITICAL(&b->ring_mux);
    if (len) *len = got;
    return 0;
}

int hal_i2c_slave_send(int bus, const uint8_t *buf, size_t len) {
    int port = hal_i2c_port(bus);
    if (port < 0) return -EINVAL;
    hal_i2c_esp32_bus_t *b = &s_bus[bus];
    if (!b->slave || b->slave_dev == NULL) return -EIO;

    /* Pre-load the response for the master's next read. The S3 has no
     * read-request ISR, so this is the only point the data can be staged. */
    esp_err_t err = i2c_slave_transmit(b->slave_dev, buf, (int)len, 10);
    if (err != ESP_OK) return -EIO;
    return (int)len;
}

void hal_i2c_slave_disable(int bus) {
    int port = hal_i2c_port(bus);
    if (port < 0) return;
    hal_i2c_esp32_bus_t *b = &s_bus[bus];
    if (!b->slave) return;

    /* Stop the worker before deleting the device so it never re-arms a freed
     * handle. The worker clears b->rx_task and self-deletes on the next loop. */
    b->rx_run = 0;
    if (b->rx_signal) xSemaphoreGive(b->rx_signal);
    for (int i = 0; i < 50 && b->rx_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (b->rx_signal) {
        vSemaphoreDelete(b->rx_signal);
        b->rx_signal = NULL;
    }

    if (b->slave_dev) (void)i2c_del_slave_device(b->slave_dev);
    if (b->sda >= 0) (void)gpio_reset_pin((gpio_num_t)b->sda);
    if (b->scl >= 0) (void)gpio_reset_pin((gpio_num_t)b->scl);
    b->slave = 0;
    b->slave_dev = NULL;
    b->sda = -1;
    b->scl = -1;
    b->ring_head = 0;
    b->ring_tail = 0;
    b->pending_len = 0;
}
