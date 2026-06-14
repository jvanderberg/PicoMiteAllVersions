/*
 * hal/hal_spi_lcd_bus.h - SPI LCD bus transport contract.
 *
 * The narrow seam between SPI LCD controller knowledge (init sequences,
 * address windows, orientation - bytes on the wire) and the MCU bus
 * mechanics that deliver those bytes. Controller code calls these entry
 * points; each port links exactly one implementation:
 *
 *   - drivers/spi_lcd/spi_lcd_bus_pico.c: the Pico shared-bus arbiter.
 *     begin() claims the multiplexed system SPI (LCD/touch/SD share it)
 *     and reconfigures speed/format for the active DISPLAY_TYPE before
 *     asserting CS; SPI0 / SPI1 / bit-bang senders are selected by pin
 *     mapping at runtime.
 *   - ports/esp32_s3/main/esp32_ili9341_lcd.c: ESP-IDF spi_master
 *     transactions on a dedicated bus. CS is hardware-managed per
 *     transaction, so begin()/end() are no-ops and dc() latches the
 *     D/C level applied to subsequent write() transactions.
 *
 * A begin()..end() pair brackets one controller transaction group (a
 * command and its parameters, or a pixel burst). dc() selects the
 * data/command line level for following writes: 0 = command, 1 = data.
 *
 * Controller-specific polarity quirks (e.g. ST7920's inverted CS) stay
 * with the controller code that knows about them, not in this contract.
 *
 * read() returns 0 when the transport cannot read the panel back (the
 * caller falls back to its shadow/framebuffer state), 1 on success.
 */

#ifndef HAL_SPI_LCD_BUS_H
#define HAL_SPI_LCD_BUS_H

#include <stdint.h>
#include <stddef.h>

void hal_spi_lcd_bus_begin(void);
void hal_spi_lcd_bus_end(void);
void hal_spi_lcd_bus_dc(int data);
void hal_spi_lcd_bus_write(const uint8_t * buf, size_t len);
int hal_spi_lcd_bus_read(uint8_t * buf, size_t len);

/* Bracket a panel-RAM read: the transport may need to drop to a slower
 * read clock between these (panels can't be read at full write speed).
 * read_begin() is called after the read region/command is set up (CS held)
 * and read_end() after the last read(), before CS is released. Ports with a
 * dedicated slow read path (e.g. a separate low-clock device) leave these
 * no-ops. */
void hal_spi_lcd_bus_read_begin(void);
void hal_spi_lcd_bus_read_end(void);

#endif /* HAL_SPI_LCD_BUS_H */
