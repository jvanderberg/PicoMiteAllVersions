/*
 * hal/hal_adc.h — ADC conversion HAL.
 *
 * Single-conversion surface for the BASIC PIN() analog read path plus the
 * capture-session surface behind CMD ADC. Channel binding rides the
 * existing hal_pin surface (hal_pin_adc_init / hal_pin_adc_select /
 * hal_pin_adc_validate); this header owns the sample clock, the conversion
 * itself, and the streaming machinery (FIFO + DMA + completion IRQ on the
 * RP backend).
 *
 * Capture model: BASIC opens a session over 1..4 channels sampled
 * round-robin at a total conversion rate. Two session shapes exist:
 *   - continuous (ADC RUN): 8-bit samples stream into two caller-owned
 *     buffers alternately, forever, with a callback from IRQ context each
 *     time a buffer fills;
 *   - single-shot (ADC START): raw 12-bit samples fill one caller-owned
 *     buffer once; the caller polls hal_adc_capture_complete.
 * The caller owns every buffer and all sample post-processing (scaling to
 * BASIC floats); the backend only moves raw samples.
 *
 * Clock model: the backend keeps one "default" sample-clock snapshot. Boot
 * programs it via hal_adc_set_default_clock; capture sessions retune the
 * divider with hal_adc_set_clock; a single-conversion caller invokes
 * hal_adc_restore_default_clock first, which reprograms the default only
 * when the divider no longer matches the snapshot.
 *
 * Global HAL conventions apply (see hal/CONTRACT.md): the impl never calls
 * MMBasic's error().
 */

#ifndef HAL_ADC_H
#define HAL_ADC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Program the ADC sample clock to `frequency` conversions per second. */
void hal_adc_set_clock(float frequency);

/* Program `frequency` and snapshot the resulting divider as the default
 * that hal_adc_restore_default_clock re-establishes. */
void hal_adc_set_default_clock(float frequency);

/* If the divider no longer matches the default snapshot (a capture session
 * retuned it), reprogram the default sample clock. No-op otherwise. */
void hal_adc_restore_default_clock(void);

/* One conversion on the channel bound via hal_pin_adc_select. Returns the
 * raw 12-bit result (0..4095), or -1 when the converter flagged an error
 * (the backend clears the sticky error state before returning). */
int32_t hal_adc_read_single(void);

/* ---- Capture sessions (CMD ADC) ---- */

/* True when `frequency` (total conversions per second across all
 * round-robin channels) is representable by the sample-clock divider. */
bool hal_adc_clock_valid(float frequency);

/* Invoked from the capture IRQ each time a continuous-mode buffer fills.
 * The callback body must be IRQ-safe and RAM-resident on XIP targets. */
typedef void (*hal_adc_buffer_callback)(void);

/* Continuous double-buffered capture: sample `nchannels` channels
 * round-robin at `frequency`, streaming 8-bit samples into buf1 and buf2
 * alternately (`nbytes` each, repeating until hal_adc_capture_end).
 * `on_buffer_done` runs in IRQ context after each buffer completes. */
void hal_adc_capture_run(int nchannels, float frequency, uint8_t * buf1, uint8_t * buf2, uint32_t nbytes, hal_adc_buffer_callback on_buffer_done);

/* Continuous-mode progress: 1 / 2 = the buffer armed to be filled next,
 * 0 = no continuous session. */
int hal_adc_capture_buffer(void);

/* Single-shot capture: sample `nchannels` channels round-robin at
 * `frequency`, one transfer of `count` raw 12-bit samples into buf. */
void hal_adc_capture_start(int nchannels, float frequency, uint16_t * buf, uint32_t count);

/* Single-shot completion poll: false while transfers are outstanding; on
 * completion stops the converter, drains the sample FIFO and returns
 * true. Poll in a loop to block, or once per interrupt-check tick. */
bool hal_adc_capture_complete(void);

/* Tear down any capture session: completion IRQ off, transfers aborted,
 * round-robin cleared, sample clock back to the default. Safe to call
 * with no session active. */
void hal_adc_capture_end(void);

#ifdef __cplusplus
}
#endif

#endif /* HAL_ADC_H */
