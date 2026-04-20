#ifndef PICOMITE_GPIO_IRQ_H
#define PICOMITE_GPIO_IRQ_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Drop-in replacement for gpio_set_irq_enabled_with_callback.
 *
 * Installs a RAM-resident dispatcher on IO_IRQ_BANK0 on first call;
 * subsequent calls just flip per-pin enable bits. The callback is
 * always External.c's gpio_callback (there is no other callback in
 * PicoMite), which is why there is no callback parameter.
 *
 * Must be used instead of the SDK's gpio_set_irq_enabled_with_callback:
 * the SDK's default dispatcher lives in flash and would hang the chip
 * if a GPIO IRQ fires during a flash write.
 */
void picomite_gpio_irq_set_enabled(unsigned int gpio, uint32_t events, bool enabled);

#ifdef __cplusplus
}
#endif

#endif /* PICOMITE_GPIO_IRQ_H */
