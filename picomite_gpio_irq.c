/*
 * picomite_gpio_irq.c — RAM-resident GPIO IRQ dispatcher.
 *
 * Replaces the SDK's gpio_default_irq_handler (which lives in flash)
 * with our own copy that lives in SRAM. PicoMite writes to flash at
 * runtime (SAVE, firmware update), during which XIP is disabled and
 * any flash-resident interrupt handler becomes unreachable — a GPIO
 * IRQ firing at that moment would hang the chip.
 *
 * Historically this was solved by patching the SDK's gpio.c to wrap
 * gpio_default_irq_handler with __not_in_flash_func(), requiring every
 * developer and every CI run to apply the patch over SDK 2.2.0. We
 * replace that with a drop-in that owns IO_IRQ_BANK0 outright, so the
 * SDK tree can stay stock.
 *
 * Design simplifications vs. the SDK's default:
 *   - Single global callback (External.c:gpio_callback) — no per-core
 *     callbacks[] array.
 *   - No raw-IRQ mask: PicoMite never uses gpio_add_raw_irq_handler_*.
 *   - Exclusive handler on IO_IRQ_BANK0 (shared-handler composability
 *     is unused).
 *
 * If any of those assumptions changes, revisit this file.
 */

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/io_bank0.h"
#include "pico/platform.h"

#include "picomite_gpio_irq.h"

/* Forward declaration: the real definition lives in External.c. We
 * avoid pulling in External.h because it depends on MMBasic's type
 * context (MMFLOAT, NBRPINS, etc.) that this translation unit has no
 * business knowing about. */
extern void gpio_callback(unsigned int gpio, uint32_t events);

static void __not_in_flash_func(picomite_gpio_irq_dispatch)(void) {
    io_bank0_irq_ctrl_hw_t *ctrl = get_core_num()
        ? &io_bank0_hw->proc1_irq_ctrl
        : &io_bank0_hw->proc0_irq_ctrl;
    for (uint gpio = 0; gpio < NUM_BANK0_GPIOS; gpio += 8) {
        uint32_t events8 = ctrl->ints[gpio >> 3u];
        for (uint i = gpio; events8 && i < gpio + 8; i++) {
            uint32_t events = events8 & 0xfu;
            if (events) {
                gpio_acknowledge_irq(i, events);
                gpio_callback(i, events);
            }
            events8 >>= 4;
        }
    }
}

static bool dispatch_installed = false;

void __not_in_flash_func(picomite_gpio_irq_set_enabled)(unsigned int gpio,
                                                        uint32_t events,
                                                        bool enabled) {
    if (!dispatch_installed) {
        irq_set_exclusive_handler(IO_IRQ_BANK0, picomite_gpio_irq_dispatch);
        dispatch_installed = true;
    }
    gpio_set_irq_enabled(gpio, events, enabled);
    if (enabled) irq_set_enabled(IO_IRQ_BANK0, true);
}
