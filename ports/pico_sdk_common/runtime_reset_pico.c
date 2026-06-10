/*
 * Pico SDK runtime hardware reset hooks used by Commands.c::do_end().
 * Core asks for intent; this file owns the watchdog/DMA details.
 */

#include <stdint.h>

#include "hardware/irq.h"
#include "hardware/structs/watchdog.h"

#include "drivers/pio_rp2/pio_rp2.h"
#include "hal/hal_adc.h"

void port_runtime_disable_watchdog(void) {
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
}

void port_runtime_abort_dma(void) {
    /* ADC capture DMA (incl. its DMA_IRQ_1 completion IRQ) is owned by
     * the hal_adc backend. */
    hal_adc_capture_end();

    /* The PIO RX/TX DMA channel pairs are owned by the PIO driver. */
    pio_rp2_dma_abort();
}
