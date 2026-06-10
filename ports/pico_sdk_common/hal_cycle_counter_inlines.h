/*
 * ports/pico_sdk_common/hal_cycle_counter_inlines.h — SysTick-backed cycle
 * countdown for pico-sdk targets.
 *
 * Pulled in by hal/hal_cycle_counter.h's trailing
 * `#include "hal_cycle_counter_inlines.h"`; this directory is on the
 * compiler's -I path for every device build.
 *
 * pico_boot.c arms SysTick at boot (CSR = ENABLE | CLKSOURCE, RVR =
 * 0x00FFFFFF) so the counter free-runs at the CPU clock. Writing any value
 * to CVR clears it; the counter reloads from RVR on the next clock, which
 * is how hal_cycle_restart re-arms the countdown.
 */

#ifndef HAL_CYCLE_COUNTER_INLINES_H
#define HAL_CYCLE_COUNTER_INLINES_H

#include <stdint.h>

#include "hardware/structs/systick.h"

static inline uint32_t hal_cycle_reload(void) {
    return 0x00FFFFFFu;
}

static inline void hal_cycle_restart(void) {
    systick_hw->cvr = 0;
}

static inline uint32_t hal_cycle_remaining(void) {
    return systick_hw->cvr;
}

#endif /* HAL_CYCLE_COUNTER_INLINES_H */
