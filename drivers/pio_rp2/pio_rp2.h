/*
 * drivers/pio_rp2/pio_rp2.h — core-visible surface of the RP2 PIO driver.
 *
 * The driver owns the BASIC PIO module wholesale: the PIO assembler, the
 * cmd_pio/fun_pio token entry points (registered in AllCommands.h), the
 * PIO DMA channels, and every pio_sm_* / dma_* register access. Core code
 * reaches the hardware only through the queries below; the BASIC interrupt
 * dispatch itself stays in core.
 */
#ifndef PIO_RP2_H
#define PIO_RP2_H

#ifdef __cplusplus
extern "C" {
#endif

/* Poll the PIO state-machine FIFOs and the PIO DMA channels for a pending
 * BASIC interrupt. Returns the interrupt target to dispatch (having already
 * cleared/disabled the source exactly as dispatch requires) or NULL.
 * Called from checkdetailinterrupts(). */
char * pio_rp2_pending_interrupt(void);

/* MM.INFO(PIO RX DMA) / MM.INFO(PIO TX DMA): is the transfer still running? */
int pio_rp2_dma_rx_busy(void);
int pio_rp2_dma_tx_busy(void);

/* Abort the four PIO DMA channels (main + control pair in each direction). */
void pio_rp2_dma_abort(void);

/* Program-boundary teardown: clear the assembler state, the PIO interrupt
 * tables and the DMA completion hooks, then abort the PIO DMA channels.
 * Called from ClearExternalIO(). */
void pio_rp2_teardown(void);

#ifdef __cplusplus
}
#endif

#endif /* PIO_RP2_H */
