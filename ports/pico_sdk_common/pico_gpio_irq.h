#ifndef PICO_GPIO_IRQ_H
#define PICO_GPIO_IRQ_H

#include <stdint.h>
#include <stdbool.h>

#include "hal/hal_pin.h" /* HAL_PIN_EDGE_* — the edge mask vocabulary */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pico SDK port replacement for gpio_set_irq_enabled_with_callback.
 *
 * Installs a RAM-resident dispatcher on IO_IRQ_BANK0 on first call;
 * subsequent calls just flip per-pin enable bits. The callback is
 * always External.c's gpio_callback (there is no other callback in this
 * firmware image), which is why there is no callback parameter.
 *
 * Must be used instead of the SDK's gpio_set_irq_enabled_with_callback:
 * the SDK's default dispatcher lives in flash and would hang the chip
 * if a GPIO IRQ fires during a flash write.
 *
 * `edge_mask` is a HAL_PIN_EDGE_* bitmask (hal/hal_pin.h); the
 * translation to the SDK's GPIO_IRQ_EDGE_* event bits happens inside
 * the implementation. HAL and SDK edge values are NOT numerically
 * equal — callers must not pass raw SDK constants.
 */
void pico_gpio_irq_set_enabled(unsigned int gpio, uint32_t edge_mask, bool enabled);

/* Raise IO_IRQ_BANK0 to the highest NVIC priority (0). The counting
 * inputs (SETPIN CIN/FIN/PIN) call this so edge counts stay accurate
 * under load from lower-priority interrupt work. */
void pico_gpio_irq_set_highest_priority(void);

#ifdef __cplusplus
}
#endif

#endif /* PICO_GPIO_IRQ_H */
