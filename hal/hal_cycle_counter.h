/*
 * hal/hal_cycle_counter.h — free-running CPU-cycle countdown for bit-banged
 * timing loops.
 *
 * The counter decrements once per CPU clock from `hal_cycle_reload()` to 0,
 * then wraps back to the reload value and keeps running.
 * `hal_cycle_restart()` re-arms the countdown at the reload value. Callers
 * time an interval by restarting and busy-waiting while
 * `hal_cycle_remaining()` is above a precomputed threshold expressed in
 * countdown coordinates (`hal_cycle_reload() + fudge - cycles`). A wait must
 * complete within one reload period — the counter gives no wrap indication.
 * On pico-sdk targets this is the Cortex-M SysTick counter: 24-bit, clocked
 * from the processor clock, so the usable window is 2^24 CPU cycles
 * (~45 ms at 378 MHz).
 *
 * There is one counter and restarting it is destructive to any countdown in
 * progress. Every timing loop (SERIN/SEROUT bit-bang, DEVICE BITSTREAM, the
 * touch-controller delay, the WS2812 bit-banger) restarts before waiting and
 * runs as an uninterrupted foreground sequence, so consumers never
 * interleave; the only other reader, MM.INFO(SYSTICK), is passive.
 *
 * The whole surface is Tier-B (static inline per port — see
 * hal/CONTRACT.md): the consumers run as __not_in_flash_func and a
 * cross-section call into flash would distort the bit timing. Each port
 * directory on the compiler's -I path supplies hal_cycle_counter_inlines.h
 * with:
 *
 *   uint32_t hal_cycle_reload(void);     countdown start value
 *                                        (compile-time constant per port)
 *   void     hal_cycle_restart(void);    re-arm the countdown at reload
 *   uint32_t hal_cycle_remaining(void);  current countdown value
 *
 * The port arms the counter at boot; `hal_cycle_restart()` only re-arms it.
 * Ports without a cycle counter make restart a no-op and remaining return 0
 * so timing loops fall through instantly instead of hanging.
 *
 * Global HAL conventions apply (see hal/CONTRACT.md).
 */

#ifndef HAL_CYCLE_COUNTER_H
#define HAL_CYCLE_COUNTER_H

#include <stdint.h>

#include "hal_cycle_counter_inlines.h"

#endif /* HAL_CYCLE_COUNTER_H */
