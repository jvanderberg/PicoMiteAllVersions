/*
 * drivers/spi_lcd/spi_lcd_bus_pico.c - Pico hal_spi_lcd_bus implementation.
 *
 * Thin adapter over the shared-bus arbiter that already lives in
 * spi_lcd.c: SetCS() claims the multiplexed system SPI (reconfiguring
 * speed/format for the active DISPLAY_TYPE via SPISpeedSet) and asserts
 * CS; lcd_xmit_byte_multi / lcd_rcvr_byte_multi are the runtime-selected
 * SPI0 / SPI1 / bit-bang senders. Controller code migrating out of
 * spi_lcd.c (the Track B extraction) calls the hal_spi_lcd_bus_* names;
 * the legacy call sites inside spi_lcd.c keep using the primitives
 * directly until they move.
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "hal/hal_spi_lcd_bus.h"

#if !HAL_PORT_IS_VGA

#include "hardware/gpio.h"

void hal_spi_lcd_bus_begin(void) {
    SetCS();
}

void hal_spi_lcd_bus_end(void) {
    ClearCS(Option.LCD_CS);
}

void hal_spi_lcd_bus_dc(int data) {
    gpio_put(LCD_CD_PIN, data ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void hal_spi_lcd_bus_write(const uint8_t * buf, size_t len) {
    lcd_xmit_byte_multi(buf, (int)len);
}

int hal_spi_lcd_bus_read(uint8_t * buf, size_t len) {
    if (!lcd_rcvr_byte_multi) return 0;
    lcd_rcvr_byte_multi(buf, (int)len);
    return 1;
}

#endif /* !HAL_PORT_IS_VGA */
