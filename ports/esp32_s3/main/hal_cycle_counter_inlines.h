/*
 * ESP32-S3 stub for the cycle-countdown surface.
 *
 * The consumers (SERIN/SEROUT in External.c, DEVICE BITSTREAM, the
 * touch-controller delay) are not in the ESP32 build; the header exists so
 * hal/hal_cycle_counter.h resolves on this port's include path. remaining()
 * pins at 0 so any timing loop that does reach this stub falls through
 * instantly. A real implementation would emulate the countdown from the
 * Xtensa CCOUNT cycle counter (esp_cpu_get_cycle_count).
 */

#ifndef ESP32_S3_HAL_CYCLE_COUNTER_INLINES_H
#define ESP32_S3_HAL_CYCLE_COUNTER_INLINES_H

#include <stdint.h>

static inline uint32_t hal_cycle_reload(void) {
    return 0x00FFFFFFu;
}

static inline void hal_cycle_restart(void) {
}

static inline uint32_t hal_cycle_remaining(void) {
    return 0u;
}

#endif /* ESP32_S3_HAL_CYCLE_COUNTER_INLINES_H */
