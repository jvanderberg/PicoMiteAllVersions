/*
 * ports/host_native/hal_cycle_counter_inlines.h — host stub for the
 * cycle-countdown surface.
 *
 * No host code path bit-bangs pins: the consumers (SERIN/SEROUT in
 * External.c, DEVICE BITSTREAM, the touch-controller delay) are compiled
 * only on device ports. remaining() pins at 0 so any timing loop that does
 * reach this stub falls through instantly instead of hanging.
 */

#ifndef HAL_CYCLE_COUNTER_INLINES_H
#define HAL_CYCLE_COUNTER_INLINES_H

#include <stdint.h>

static inline uint32_t hal_cycle_reload(void) {
    return 0x00FFFFFFu;
}

static inline void hal_cycle_restart(void) {
}

static inline uint32_t hal_cycle_remaining(void) {
    return 0u;
}

#endif /* HAL_CYCLE_COUNTER_INLINES_H */
