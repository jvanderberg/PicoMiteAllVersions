/*
 * hal_adc_pico.c — RP2040 / RP2350 ADC backend (hal_adc.h).
 *
 * Owns the sample-clock divider math, the raw conversion path for the
 * BASIC PIN() analog read, and the CMD ADC capture-session machinery
 * (round-robin + FIFO setup, the chained two-channel DMA that implements
 * continuous double-buffering, and the DMA_IRQ_1 completion handler).
 * The default-clock snapshot lets the read path detect when a capture
 * session retuned the divider and restore the boot-time 500 kHz rate
 * before converting.
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"

#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/structs/adc.h"

#include "hal/hal_adc.h"

/* DMA channel assignments for capture sessions (ADC_DMA / ADC_DMA2 from
 * configuration.h). The data channel streams FIFO samples; the control
 * channel re-arms it for continuous mode. */
static const uint32_t ADC_dma_chan = ADC_DMA;
static const uint32_t ADC_dma_chan2 = ADC_DMA2;

/* Continuous-mode double-buffer state. adc_buf_next is the buffer the
 * control DMA channel writes into the data channel's trigger register on
 * the next re-arm; the IRQ handler flips it between adc_buf1/adc_buf2. */
static volatile uint8_t * adc_buf_next = NULL;
static uint8_t * adc_buf1 = NULL;
static uint8_t * adc_buf2 = NULL;
static hal_adc_buffer_callback adc_buffer_done = NULL;

/* Divider value programmed by hal_adc_set_default_clock; compared against
 * the live register to detect a capture-session retune. */
static int hal_adc_default_div;
static float hal_adc_default_freq = 500000.0f;

void hal_adc_set_clock(float frequency) {
    //Our ADC clock is running at ADC_CLK_SPEED (in Hz) so we need to stretch the time to produce the required frequency
    //The time delta for our frequency is 1/frequency*96 - 1/(ADC_CLK_SPEED) seconds
    //This must be added to clk_div as a number of CPU clock ticks i.e. delta/(1/ADC_CLK_SPEED)
    //Plus we need to add 95 to cater for the actual time taken for the conversion
    double delta = 1.0 / (frequency * 96.0) - 1.0 / ((double)ADC_CLK_SPEED);
    float div = delta / (1.0 / ((double)ADC_CLK_SPEED)) * 96.0 + 95.0;
    if (div <= 96.0) div = 0;
    adc_set_clkdiv(div);
}

void hal_adc_set_default_clock(float frequency) {
    hal_adc_set_clock(frequency);
    hal_adc_default_freq = frequency;
    hal_adc_default_div = adc_hw->div;
}

void hal_adc_restore_default_clock(void) {
    if (hal_adc_default_div != adc_hw->div) {
        hal_adc_set_clock(hal_adc_default_freq);
    }
}

int32_t hal_adc_read_single(void) {
    int32_t a = adc_read();
    if (adc_hw->cs & (ADC_CS_ERR_STICKY_BITS | ADC_CS_ERR_BITS)) {
        hw_set_bits(&adc_hw->cs, ADC_CS_ERR_STICKY_BITS);
        a = -1;
    }
    return a;
}

bool hal_adc_clock_valid(float frequency) {
    return !(frequency < ADC_CLK_SPEED / 65536.0 / 96.0 || frequency > ADC_CLK_SPEED / 96.0);
}

static void __not_in_flash_func(hal_adc_dma_irq)(void) {
    // Clear the interrupt request for DMA control channel
    dma_hw->ints1 = (1u << ADC_dma_chan);
    if (adc_buf_next == adc_buf2)
        adc_buf_next = adc_buf1;
    else
        adc_buf_next = adc_buf2;
    adc_buffer_done();
}

void hal_adc_capture_run(int nchannels, float frequency, uint8_t * buf1, uint8_t * buf2, uint32_t nbytes, hal_adc_buffer_callback on_buffer_done) {
    dma_channel_cleanup(ADC_dma_chan);
    dma_channel_cleanup(ADC_dma_chan2);
    adc_init();
    adc_set_round_robin(nchannels == 1 ? 1 : nchannels == 2 ? 3
                                         : nchannels == 3   ? 7
                                                            : 15);
    adc_fifo_setup(
        true,  // Write each completed conversion to the sample FIFO
        true,  // Enable DMA data request (DREQ)
        1,     // DREQ (and IRQ) asserted when at least 1 sample present
        false, // We won't see the ERR bit because of 8 bit reads; disable.
        true   // Shift each sample to 8 bits when pushing to FIFO
    );
    adc_buf1 = buf1;
    adc_buf2 = buf2;
    adc_buffer_done = on_buffer_done;
    adc_buf_next = buf1;
    hal_adc_set_clock(frequency);
    // Set up the DMA to start transferring data as soon as it appears in FIFO
    dma_channel_config cfg = dma_channel_get_default_config(ADC_dma_chan);

    // Reading from constant address, writing to incrementing byte addresses
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_irq_quiet(&cfg, false);
    channel_config_set_dreq(&cfg, DREQ_ADC);
    channel_config_set_chain_to(&cfg, ADC_dma_chan2);
    dma_channel_configure(ADC_dma_chan, &cfg,
                          adc_buf_next,  // dst
                          &adc_hw->fifo, // src
                          nbytes,        // transfer count
                          false          // start immediately
    );
    dma_channel_config c2 = dma_channel_get_default_config(ADC_dma_chan2); //Get configurations for control channel
    channel_config_set_transfer_data_size(&c2, DMA_SIZE_32);               //Set control channel data transfer size to 32 bits
    channel_config_set_read_increment(&c2, false);                         //Set control channel read increment to false
    channel_config_set_write_increment(&c2, false);                        //Set control channel write increment to false
    channel_config_set_dreq(&c2, 0x3F);
    dma_channel_configure(ADC_dma_chan2,
                          &c2,
                          &dma_hw->ch[ADC_dma_chan].al2_write_addr_trig,
                          &adc_buf_next,
                          1,
                          false); //Configure control channel
    dma_channel_set_irq1_enabled(ADC_dma_chan, true);

    // set DMA IRQ handler
    irq_set_exclusive_handler(DMA_IRQ_1, hal_adc_dma_irq);
    // set highest IRQ priority
    irq_set_enabled(DMA_IRQ_1, true);
    dma_start_channel_mask(1u << ADC_dma_chan2);
    adc_run(true);
    adc_buf_next = buf2;
}

int hal_adc_capture_buffer(void) {
    return ((adc_buf_next == adc_buf1 && adc_buf_next) ? 1 : ((adc_buf_next == adc_buf2 && adc_buf_next) ? 2 : 0));
}

void hal_adc_capture_start(int nchannels, float frequency, uint16_t * buf, uint32_t count) {
    adc_init();
    adc_set_round_robin(nchannels == 1 ? 1 : nchannels == 2 ? 3
                                         : nchannels == 3   ? 7
                                                            : 15);
    adc_fifo_setup(
        true,  // Write each completed conversion to the sample FIFO
        true,  // Enable DMA data request (DREQ)
        1,     // DREQ (and IRQ) asserted when at least 1 sample present
        false, // We won't see the ERR bit because of 8 bit reads; disable.
        false  // Shift each sample to 8 bits when pushing to FIFO
    );
    hal_adc_set_clock(frequency);
    // Set up the DMA to start transferring data as soon as it appears in FIFO
    dma_channel_config cfg = dma_channel_get_default_config(ADC_dma_chan);

    // Reading from constant address, writing to incrementing byte addresses
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_irq_quiet(&cfg, true);

    // Pace transfers based on availability of ADC samples
    channel_config_set_dreq(&cfg, DREQ_ADC);

    dma_channel_configure(ADC_dma_chan, &cfg,
                          (uint8_t *)buf, // dst
                          &adc_hw->fifo,  // src
                          count,          // transfer count
                          true            // start immediately
    );
    adc_run(true);
}

bool hal_adc_capture_complete(void) {
    if (dma_channel_is_busy(ADC_dma_chan)) return false;
    __compiler_memory_barrier();
    // Stop any new conversions from starting, and clean up the FIFO in
    // case the ADC was still mid-conversion.
    adc_run(false);
    adc_fifo_drain();
    return true;
}

void hal_adc_capture_end(void) {
    irq_set_enabled(DMA_IRQ_1, false);
    dma_hw->abort = ((1u << ADC_dma_chan2) | (1u << ADC_dma_chan));
    if (dma_channel_is_busy(ADC_dma_chan)) dma_channel_abort(ADC_dma_chan);
    if (dma_channel_is_busy(ADC_dma_chan2)) dma_channel_abort(ADC_dma_chan2);
    adc_set_round_robin(0);
    hal_adc_set_clock(hal_adc_default_freq);
    adc_buf_next = adc_buf1 = adc_buf2 = NULL;
    adc_buffer_done = NULL;
}
