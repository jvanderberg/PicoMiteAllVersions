/*
 * hal_i2c_pico.c — RP2040 / RP2350 I²C backend (hal_i2c.h).
 *
 * Owns the pico-SDK master transactions and the register/IRQ-driven slave
 * for the BASIC I2C / I2C2 commands. The shared bus driver
 * (drivers/i2c_bus/I2C.c) parses the BASIC syntax, manages the send/receive
 * buffers and the MMBasic option/interrupt state, then calls only the
 * hal_i2c_* surface here.
 *
 * Bus mapping: bus 0 -> i2c0 (BASIC I2C), bus 1 -> i2c1 (BASIC I2C2).
 *
 * Slave model: the contract is poll/send, the hardware is IRQ-driven. The
 * IRQ handlers stay internal to this file; they raise the slave
 * receive/send "ready" events directly in the shared I2C status words
 * (I2C_Status / I2C2_Status) that the interrupt dispatcher in MM_Misc.c
 * watches. hal_i2c_slave_poll performs the buffered raw receive and
 * hal_i2c_slave_send the buffered raw transmit.
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"

#include "hal/hal_i2c.h"
#include "I2C.h"

/* Shared I²C status words owned by I2C.c. The slave IRQ raises the
 * receive/send "ready" event bits here for the MMBasic interrupt
 * dispatcher. */
extern volatile unsigned int I2C_Status;
extern volatile unsigned int I2C2_Status;

/* Bounded-wait counter incremented by the 1 ms tick; used to limit the
 * multi-byte slave receive drain. */
extern volatile unsigned int I2CTimer;

static i2c_inst_t * hal_i2c_inst(int bus) {
    if (bus == 0) return i2c0;
    if (bus == 1) return i2c1;
    return NULL;
}

int hal_i2c_init(void) {
    return 0;
}

int hal_i2c_master_init(int bus, int sda_gpio, int scl_gpio, uint32_t baud) {
    i2c_inst_t * inst = hal_i2c_inst(bus);
    if (inst == NULL) return -1;
    i2c_init(inst, baud);
    gpio_pull_up(sda_gpio);
    gpio_pull_up(scl_gpio);
    return 0;
}

void hal_i2c_master_deinit(int bus) {
    i2c_inst_t * inst = hal_i2c_inst(bus);
    if (inst == NULL) return;
    i2c_deinit(inst);
}

int hal_i2c_master_write(int bus, uint8_t addr, const uint8_t * buf, size_t len,
                         int nostop, uint32_t timeout_us) {
    i2c_inst_t * inst = hal_i2c_inst(bus);
    if (inst == NULL) return -1;
    int ret = i2c_write_timeout_us(inst, addr, (uint8_t *)buf, len,
                                   nostop ? true : false, timeout_us);
    if (ret == PICO_ERROR_GENERIC) return -2;
    if (ret == PICO_ERROR_TIMEOUT) return -3;
    return ret;
}

int hal_i2c_master_read(int bus, uint8_t addr, uint8_t * buf, size_t len,
                        int nostop, uint32_t timeout_us) {
    i2c_inst_t * inst = hal_i2c_inst(bus);
    if (inst == NULL) return -1;
    int ret = i2c_read_timeout_us(inst, addr, buf, len,
                                  nostop ? true : false, timeout_us);
    if (ret == PICO_ERROR_GENERIC) return -2;
    if (ret == PICO_ERROR_TIMEOUT) return -3;
    return ret;
}

/* ---------------------------------------------------------------------- */
/* Slave role                                                             */
/* ---------------------------------------------------------------------- */

void __not_in_flash_func(i2c0_irq_handler)(void) {
    // Get interrupt status
    uint32_t status = i2c0->hw->intr_stat;
    //is a write request? Or a read request ? event
    if (status & I2C_IC_INTR_STAT_R_RX_FULL_BITS) {
        i2c0->hw->intr_mask = I2C_IC_INTR_MASK_M_RD_REQ_BITS;
        I2C_Status |= I2C_Status_Slave_Receive_Rdy;
    } else if (status & I2C_IC_INTR_STAT_R_RD_REQ_BITS) {
        i2c0->hw->clr_rd_req;
        I2C_Status |= I2C_Status_Slave_Send_Rdy;
    }
}

void __not_in_flash_func(i2c1_irq_handler)(void) {
    // Get interrupt status
    uint32_t status = i2c1->hw->intr_stat;
    //is a write request? Or a read request ? event
    if (status & I2C_IC_INTR_STAT_R_RX_FULL_BITS) {
        i2c1->hw->intr_mask = I2C_IC_INTR_MASK_M_RD_REQ_BITS;
        I2C2_Status |= I2C_Status_Slave_Receive_Rdy;
    } else if (status & I2C_IC_INTR_STAT_R_RD_REQ_BITS) {
        i2c1->hw->clr_rd_req;
        I2C2_Status |= I2C_Status_Slave_Send_Rdy;
    }
}

int hal_i2c_slave_enable(int bus, uint8_t addr) {
    if (bus == 0) {
        gpio_pull_up(PinDef[I2C0SDApin].GPno);
        gpio_pull_up(PinDef[I2C0SCLpin].GPno);
        i2c_init(i2c0, 400000);
        i2c_set_slave_mode(i2c0, true, addr);
        // Enable the I2C interrupts we want to process
        i2c0->hw->intr_mask = I2C_IC_INTR_STAT_R_RX_FULL_BITS | I2C_IC_INTR_MASK_M_RD_REQ_BITS;
        // Set up the interrupt handler to service I2C interrupts
        irq_set_exclusive_handler(I2C0_IRQ, i2c0_irq_handler);
        irq_set_enabled(I2C0_IRQ, true);
        return 0;
    }
    if (bus == 1) {
        gpio_pull_up(PinDef[I2C1SDApin].GPno);
        gpio_pull_up(PinDef[I2C1SCLpin].GPno);
        i2c_init(i2c1, 400000);
        i2c_set_slave_mode(i2c1, true, addr);
        i2c1->hw->intr_mask = I2C_IC_INTR_STAT_R_RX_FULL_BITS | I2C_IC_INTR_MASK_M_RD_REQ_BITS;
        irq_set_exclusive_handler(I2C1_IRQ, i2c1_irq_handler);
        irq_set_enabled(I2C1_IRQ, true);
        return 0;
    }
    return -1;
}

int hal_i2c_slave_poll(int bus, uint8_t * buf, size_t cap, size_t * len) {
    int count = 1;
    i2c_inst_t * inst = hal_i2c_inst(bus);
    if (inst == NULL) {
        if (len) *len = 0;
        return -1;
    }
    i2c_read_raw_blocking(inst, buf, 1);
    if (cap > 1) {
        I2CTimer = 0;
        while ((size_t)count < cap && I2CTimer < cap / 10 + 2) {
            if (inst->hw->status & 8) i2c_read_raw_blocking(inst, &buf[count++], 1);
        }
    }
    inst->hw->intr_mask = I2C_IC_INTR_STAT_R_RX_FULL_BITS | I2C_IC_INTR_MASK_M_RD_REQ_BITS;
    if (len) *len = (size_t)count;
    return 0;
}

int hal_i2c_slave_send(int bus, const uint8_t * buf, size_t len) {
    i2c_inst_t * inst = hal_i2c_inst(bus);
    if (inst == NULL) return -1;
    i2c_write_raw_blocking(inst, (uint8_t *)buf, len);
    return 0;
}

void hal_i2c_slave_disable(int bus) {
    if (bus == 0) {
        irq_set_enabled(I2C0_IRQ, false);
        irq_remove_handler(I2C0_IRQ, i2c0_irq_handler);
        i2c_set_slave_mode(i2c0, false, 0);
        i2c0->hw->intr_mask = 0;
        return;
    }
    if (bus == 1) {
        irq_set_enabled(I2C1_IRQ, false);
        irq_remove_handler(I2C1_IRQ, i2c1_irq_handler);
        i2c_set_slave_mode(i2c1, false, 0);
        i2c1->hw->intr_mask = 0;
        return;
    }
}
