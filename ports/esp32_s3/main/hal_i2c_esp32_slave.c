/*
 * hal_i2c_esp32_slave.c — ESP32-S3 register-level I²C slave backend
 * (hal_i2c.h).
 *
 * Slave role is driven at register level (hal/i2c_ll.h) with an interrupt
 * handler owned by this backend; the ESP-IDF i2c-slave driver is not used.
 * A given IDF port is either a master or a slave at one time; enabling
 * slave mode first tears down the master bus on that port (so I2C SLAVE
 * OPEN on bus 0 is incompatible with the Freenove touch, which holds the
 * bus-0 master). The slave SDA/SCL GPIOs come from PinDef[] via the
 * I2C0SDApin / I2C1SDApin assignments that the shared bus driver configures
 * with ExtCfg() before it calls hal_i2c_slave_enable.
 *
 * Slave receive: the ISR services the RX-FIFO watermark
 * (I2C_RXFIFO_WM_INT) and transaction-complete (I2C_TRANS_COMPLETE_INT)
 * interrupts, draining the 32-byte hardware RX FIFO into a per-bus ring on
 * each event. On transaction complete with data received, it raises the
 * receive-ready status bit (I2C_Status_Slave_Receive_Rdy in I2C_Status for
 * bus 0, I2C2_Status for bus 1) that the MMBasic interrupt dispatcher
 * watches, so a BASIC `I2C SLAVE OPEN` receive interrupt subroutine fires.
 * hal_i2c_slave_poll drains the ring. A zero-length master write (address
 * with no data bytes) raises no event: the receive path is data-driven,
 * matching the RP backend.
 *
 * Slave send: hal_i2c_slave_send stages the response in a per-bus buffer,
 * writes the first 32 bytes into the TX FIFO and lets the TX-FIFO watermark
 * interrupt (I2C_TXFIFO_WM_INT) refill the FIFO from the staged buffer as
 * the master clocks it out. Each send replaces any previously staged,
 * unread response.
 *
 * ESP32-S3 send-side limit: the S3 lacks SOC_I2C_SLAVE_CAN_GET_STRETCH_CAUSE,
 * so there is no master-read-request (clock-stretch) event and the BASIC
 * slave *send* interrupt subroutine cannot fire on this silicon
 * (I2C_Status_Slave_Send_Rdy is never raised). Response data must be staged
 * with hal_i2c_slave_send before the master reads; a read with nothing
 * staged clocks out idle bytes.
 */

#include <errno.h>
#include <string.h>

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"

#include "freertos/FreeRTOS.h"

#include "driver/gpio.h"
#include "esp_intr_alloc.h"
#include "esp_private/gpio.h"
#include "esp_private/periph_ctrl.h"
#include "esp_rom_gpio.h"
#include "hal/i2c_ll.h"
#include "soc/i2c_periph.h"
#include "soc/i2c_reg.h"
#include "soc/io_mux_reg.h"

#include "hal/hal_i2c.h"
#include "hal_i2c_esp32.h"
#include "hal_i2c_esp32_internal.h"
#include "i2c_config.h"

/* FIFO watermark thresholds: the IDF slave-driver defaults (half the
 * 32-byte FIFO). RX: interrupt when the FIFO holds >= 16 bytes; TX:
 * interrupt when it has drained to < 16 bytes. */
#define HAL_I2C_ESP32_SLAVE_FIFO_WM (SOC_I2C_FIFO_LEN / 2)

/* Slave SCL timeout register value; mirrors the IDF slave driver's
 * I2C_SLAVE_TIMEOUT_DEFAULT. */
#define HAL_I2C_ESP32_SLAVE_TOUT 32000

/* Interrupt bits this backend enables and services. */
#define HAL_I2C_ESP32_SLAVE_RX_INTS \
    (I2C_RXFIFO_WM_INT_ENA_M | I2C_TRANS_COMPLETE_INT_ENA_M)
#define HAL_I2C_ESP32_SLAVE_TX_INTS (I2C_TXFIFO_WM_INT_ENA_M)
#define HAL_I2C_ESP32_SLAVE_ALL_INTS \
    (HAL_I2C_ESP32_SLAVE_RX_INTS | HAL_I2C_ESP32_SLAVE_TX_INTS)

/* Resolve the slave bus's SDA/SCL GPIO numbers from the shared pin
 * assignments. i2cSlave()/i2c2Slave() set I2C0SDApin / I2C1SDApin and call
 * ExtCfg() on them before hal_i2c_slave_enable, so PinDef[] holds the GPIO. */
static int hal_i2c_slave_pins(int bus, int * sda_gpio, int * scl_gpio) {
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
/* Slave role                                                             */
/* ---------------------------------------------------------------------- */

/* Slave interrupt handler. Bounded by construction: every FIFO read is
 * clamped to both the hardware count and the 32-byte FIFO depth, and every
 * ring/TX index is range-checked against its buffer, so no master write
 * length can overrun a buffer here. */
static void IRAM_ATTR hal_i2c_slave_isr(void * arg) {
    int bus = (int)(intptr_t)arg;
    hal_i2c_esp32_bus_t * b = &hal_i2c_esp32_bus_state[bus];
    i2c_dev_t * hw = b->hw;
    uint32_t status = 0;

    i2c_ll_get_intr_mask(hw, &status);
    i2c_ll_clear_intr_mask(hw, status);
    if (status == 0) return;

    if (status & (I2C_RXFIFO_WM_INT_ENA_M | I2C_TRANS_COMPLETE_INT_ENA_M)) {
        /* Drain whatever the RX FIFO holds (at most its 32-byte depth)
         * into the ring. */
        uint8_t chunk[SOC_I2C_FIFO_LEN];
        uint32_t cnt = 0;
        i2c_ll_get_rxfifo_cnt(hw, &cnt);
        if (cnt > SOC_I2C_FIFO_LEN) cnt = SOC_I2C_FIFO_LEN;
        if (cnt > 0) {
            i2c_ll_read_rxfifo(hw, chunk, (uint8_t)cnt);
            portENTER_CRITICAL_ISR(&b->mux);
            for (uint32_t i = 0; i < cnt; i++) {
                size_t next = (b->ring_head + 1) % HAL_I2C_ESP32_SLAVE_RING;
                if (next == b->ring_tail) break; /* full: drop the overflow. */
                b->ring[b->ring_head] = chunk[i];
                b->ring_head = next;
            }
            portEXIT_CRITICAL_ISR(&b->mux);
            b->rx_trans_len += cnt;
        }
    }

    if (status & I2C_TRANS_COMPLETE_INT_ENA_M) {
        /* STOP seen. Raise the receive-ready event only if this transaction
         * carried data (a master read also completes with rx_trans_len 0). */
        if (b->rx_trans_len > 0) {
            b->rx_trans_len = 0;
            if (bus == 0)
                I2C_Status |= I2C_Status_Slave_Receive_Rdy;
            else
                I2C2_Status |= I2C_Status_Slave_Receive_Rdy;
        }
    }

    if (status & I2C_TXFIFO_WM_INT_ENA_M) {
        /* The master has drained the TX FIFO below the watermark: refill it
         * from the staged response, and stop the refill interrupt once the
         * whole message has been handed to the hardware. */
        portENTER_CRITICAL_ISR(&b->mux);
        size_t remaining = b->tx_len - b->tx_pos;
        if (remaining > 0) {
            uint32_t space = 0;
            i2c_ll_get_txfifo_len(hw, &space);
            size_t n = (remaining < space) ? remaining : space;
            if (n > 0) {
                i2c_ll_write_txfifo(hw, &b->tx_buf[b->tx_pos], (uint8_t)n);
                b->tx_pos += n;
                remaining -= n;
            }
        }
        if (remaining == 0) {
            i2c_ll_disable_intr_mask(hw, HAL_I2C_ESP32_SLAVE_TX_INTS);
        }
        portEXIT_CRITICAL_ISR(&b->mux);
    }
}

/* Route one I²C pad through the GPIO matrix: open-drain input+output with
 * the internal pull-up, the same configuration the IDF i2c driver applies. */
static esp_err_t hal_i2c_slave_route_pin(int port, int gpio, int is_scl) {
    gpio_config_t conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_down_en = false,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pin_bit_mask = 1ULL << gpio,
    };
    esp_err_t err = gpio_set_level((gpio_num_t)gpio, 1);
    if (err != ESP_OK) return err;
    err = gpio_config(&conf);
    if (err != ESP_OK) return err;
    err = gpio_func_sel((gpio_num_t)gpio, PIN_FUNC_GPIO);
    if (err != ESP_OK) return err;
    esp_rom_gpio_connect_out_signal(gpio,
                                    is_scl ? i2c_periph_signal[port].scl_out_sig
                                           : i2c_periph_signal[port].sda_out_sig,
                                    0, 0);
    esp_rom_gpio_connect_in_signal(gpio,
                                   is_scl ? i2c_periph_signal[port].scl_in_sig
                                          : i2c_periph_signal[port].sda_in_sig,
                                   0);
    return ESP_OK;
}

int hal_i2c_slave_enable(int bus, uint8_t addr) {
    int port = hal_i2c_esp32_port(bus);
    if (port < 0) return -EINVAL;
    hal_i2c_esp32_bus_t * b = &hal_i2c_esp32_bus_state[bus];

    int sda_gpio = -1, scl_gpio = -1;
    if (hal_i2c_slave_pins(bus, &sda_gpio, &scl_gpio) != 0) return -EINVAL;

    if (b->slave) {
        hal_i2c_slave_disable(bus);
    }

    /* A port is master XOR slave. Drop the system master reference (and, if
     * it was the last one, the bus itself) so the slave can claim the port.
     * Every failure path from here re-acquires that reference, so a failed
     * enable leaves the master exactly as it was. */
    int had_master = 0;
    int master_sda = -1, master_scl = -1;
    if (b->system_holds) {
        had_master = 1;
        master_sda = hal_i2c_esp32_master_state[port].sda;
        master_scl = hal_i2c_esp32_master_state[port].scl;
        b->system_holds = 0;
        esp32_i2c_master_bus_release(port);
    }

    /* Other subsystems (Freenove touch/codec) may still hold the master bus
     * on this port; programming the slave registers under a live IDF master
     * driver corrupts it. Refuse and restore the system hold. */
    if (hal_i2c_esp32_master_state[port].bus != NULL) {
        if (had_master) {
            if (esp32_i2c_master_bus(port, master_sda, master_scl) != NULL)
                b->system_holds = 1;
        }
        return -EBUSY;
    }

    int rc = -EIO;
    i2c_dev_t * hw = I2C_LL_GET_HW(port);

    /* Module clock on + register reset, then route the pads. */
    PERIPH_RCC_ATOMIC() {
        i2c_ll_enable_bus_clock(port, true);
        i2c_ll_reset_register(port);
    }
    i2c_ll_enable_controller_clock(hw, true);

    if (hal_i2c_slave_route_pin(port, sda_gpio, 0) != ESP_OK ||
        hal_i2c_slave_route_pin(port, scl_gpio, 1) != ESP_OK) {
        goto fail_module_off;
    }

    /* Slave-mode peripheral programming: the same sequence and defaults the
     * IDF slave driver uses (7-bit address, FIFO mode, MSB-first, watermarks
     * at half FIFO, TX auto-start). */
    i2c_ll_clear_intr_mask(hw, UINT32_MAX);
    /* Direct ctr-register writes: slave mode, open-drain pads, arbitration
       off. These mirror the i2c_ll_set_mode / i2c_ll_enable_pins_open_drain /
       i2c_ll_enable_arbitration inlines, which not every IDF 5.3.x ships. */
    hw->ctr.ms_mode = 0;
    hw->ctr.sda_force_out = 1;
    hw->ctr.scl_force_out = 1;
    hw->ctr.arbitration_en = 0;
    i2c_ll_set_data_mode(hw, I2C_DATA_MODE_MSB_FIRST, I2C_DATA_MODE_MSB_FIRST);
    i2c_ll_slave_set_fifo_mode(hw, true);
    i2c_ll_enable_mem_access_nonfifo(hw, false);
    i2c_ll_set_source_clk(hw, I2C_CLK_SRC_DEFAULT);
    i2c_ll_set_slave_addr(hw, addr, false);
    i2c_ll_set_txfifo_empty_thr(hw, HAL_I2C_ESP32_SLAVE_FIFO_WM);
    i2c_ll_set_rxfifo_full_thr(hw, HAL_I2C_ESP32_SLAVE_FIFO_WM);
    i2c_ll_set_sda_timing(hw, 10, 10);
    i2c_ll_set_tout(hw, HAL_I2C_ESP32_SLAVE_TOUT);
    i2c_ll_slave_tx_auto_start_en(hw, true);
    i2c_ll_txfifo_rst(hw);
    i2c_ll_rxfifo_rst(hw);
    i2c_ll_update(hw);

    b->hw = hw;
    b->ring_head = 0;
    b->ring_tail = 0;
    b->rx_trans_len = 0;
    b->tx_len = 0;
    b->tx_pos = 0;

    if (esp_intr_alloc_intrstatus(
            i2c_periph_signal[port].irq,
            ESP_INTR_FLAG_SHARED | ESP_INTR_FLAG_LOWMED,
            (uint32_t)i2c_ll_get_interrupt_status_reg(hw),
            HAL_I2C_ESP32_SLAVE_ALL_INTS, hal_i2c_slave_isr,
            (void *)(intptr_t)bus, &b->intr) != ESP_OK) {
        goto fail_module_off;
    }

    i2c_ll_clear_intr_mask(hw, HAL_I2C_ESP32_SLAVE_ALL_INTS);
    i2c_ll_enable_intr_mask(hw, HAL_I2C_ESP32_SLAVE_RX_INTS);

    b->slave = 1;
    b->sda = sda_gpio;
    b->scl = scl_gpio;
    b->pending_len = 0;
    return 0;

fail_module_off:
    i2c_ll_enable_controller_clock(hw, false);
    PERIPH_RCC_ATOMIC() {
        i2c_ll_enable_bus_clock(port, false);
    }
    (void)gpio_reset_pin((gpio_num_t)sda_gpio);
    (void)gpio_reset_pin((gpio_num_t)scl_gpio);
    if (had_master &&
        esp32_i2c_master_bus(port, master_sda, master_scl) != NULL) {
        b->system_holds = 1;
    }
    return rc;
}

int hal_i2c_slave_poll(int bus, uint8_t * buf, size_t cap, size_t * len) {
    if (len) *len = 0;
    int port = hal_i2c_esp32_port(bus);
    if (port < 0) return -EINVAL;
    hal_i2c_esp32_bus_t * b = &hal_i2c_esp32_bus_state[bus];
    if (!b->slave) return -EIO;

    size_t got = 0;
    portENTER_CRITICAL(&b->mux);
    while (got < cap && b->ring_tail != b->ring_head) {
        buf[got++] = b->ring[b->ring_tail];
        b->ring_tail = (b->ring_tail + 1) % HAL_I2C_ESP32_SLAVE_RING;
    }
    portEXIT_CRITICAL(&b->mux);
    if (len) *len = got;
    return 0;
}

int hal_i2c_slave_send(int bus, const uint8_t * buf, size_t len) {
    int port = hal_i2c_esp32_port(bus);
    if (port < 0) return -EINVAL;
    hal_i2c_esp32_bus_t * b = &hal_i2c_esp32_bus_state[bus];
    if (!b->slave) return -EIO;
    if (len > HAL_I2C_ESP32_SLAVE_TX) return -EINVAL;
    i2c_dev_t * hw = b->hw;

    /* Stage the response for the master's next read, replacing anything
     * staged before. The S3 has no read-request event, so this is the only
     * point the data can be staged: load the FIFO now and let the watermark
     * interrupt feed it the rest as the master clocks it out. */
    portENTER_CRITICAL(&b->mux);
    i2c_ll_disable_intr_mask(hw, HAL_I2C_ESP32_SLAVE_TX_INTS);
    i2c_ll_txfifo_rst(hw);
    memcpy(b->tx_buf, buf, len);
    b->tx_len = len;
    uint32_t space = 0;
    i2c_ll_get_txfifo_len(hw, &space);
    size_t n = (len < space) ? len : space;
    if (n > 0) i2c_ll_write_txfifo(hw, b->tx_buf, (uint8_t)n);
    b->tx_pos = n;
    if (n < len) {
        i2c_ll_clear_intr_mask(hw, HAL_I2C_ESP32_SLAVE_TX_INTS);
        i2c_ll_enable_intr_mask(hw, HAL_I2C_ESP32_SLAVE_TX_INTS);
    }
    portEXIT_CRITICAL(&b->mux);
    return (int)len;
}

void hal_i2c_slave_disable(int bus) {
    int port = hal_i2c_esp32_port(bus);
    if (port < 0) return;
    hal_i2c_esp32_bus_t * b = &hal_i2c_esp32_bus_state[bus];
    if (!b->slave) return;
    i2c_dev_t * hw = b->hw;

    i2c_ll_disable_intr_mask(hw, HAL_I2C_ESP32_SLAVE_ALL_INTS);
    i2c_ll_clear_intr_mask(hw, UINT32_MAX);
    if (b->intr) {
        (void)esp_intr_free(b->intr);
        b->intr = NULL;
    }
    i2c_ll_txfifo_rst(hw);
    i2c_ll_rxfifo_rst(hw);
    i2c_ll_enable_controller_clock(hw, false);
    PERIPH_RCC_ATOMIC() {
        i2c_ll_enable_bus_clock(port, false);
    }
    if (b->sda >= 0) (void)gpio_reset_pin((gpio_num_t)b->sda);
    if (b->scl >= 0) (void)gpio_reset_pin((gpio_num_t)b->scl);
    b->slave = 0;
    b->hw = NULL;
    b->sda = -1;
    b->scl = -1;
    b->ring_head = 0;
    b->ring_tail = 0;
    b->rx_trans_len = 0;
    b->tx_len = 0;
    b->tx_pos = 0;
    b->pending_len = 0;
}
