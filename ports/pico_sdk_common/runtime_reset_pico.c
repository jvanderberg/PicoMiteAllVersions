/*
 * Pico SDK runtime hardware reset hooks used by Commands.c::do_end().
 * Core asks for intent; this file owns the watchdog/DMA details.
 */

#include <stdint.h>

#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/structs/watchdog.h"

#include "hal/hal_adc.h"

extern uint32_t dma_rx_chan;
extern uint32_t dma_rx_chan2;
extern uint32_t dma_tx_chan;
extern uint32_t dma_tx_chan2;

void port_runtime_disable_watchdog(void) {
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
}

void port_runtime_abort_dma(void) {
    /* ADC capture DMA (incl. its DMA_IRQ_1 completion IRQ) is owned by
     * the hal_adc backend. */
    hal_adc_capture_end();

    dma_hw->abort = ((1u << dma_rx_chan2) | (1u << dma_rx_chan));
    if (dma_channel_is_busy(dma_rx_chan)) dma_channel_abort(dma_rx_chan);
    if (dma_channel_is_busy(dma_rx_chan2)) dma_channel_abort(dma_rx_chan2);

    dma_hw->abort = ((1u << dma_tx_chan2) | (1u << dma_tx_chan));
    if (dma_channel_is_busy(dma_tx_chan)) dma_channel_abort(dma_tx_chan);
    if (dma_channel_is_busy(dma_tx_chan2)) dma_channel_abort(dma_tx_chan2);
}
