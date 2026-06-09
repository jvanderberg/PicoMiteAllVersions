/*
 * drivers/spi_lcd/spi_lcd_periph_io_stub.c — no-op stub of
 * hal_periph_reserve_io for VGA / HDMI / DVI-WiFi ports. The real
 * impl (spi_lcd_periph_io.c) references symbols defined in Touch.c
 * and macros from SSD1963.h that are only available when the SPI-LCD
 * + touch driver pair is linked, which excludes the VGA-family
 * ports.
 */

#include "hal/hal_periph_io.h"

void hal_periph_reserve_io(void) {}

/* SPI-LCD / SSD1963 / XPT2046 init entry-point stubs for VGA-family
 * ports. The boot path in PicoMite.c calls these unconditionally; on
 * VGA they no-op (Touch.c / SSD1963.c / spi_lcd.c bodies aren't
 * linked here). Symbols match the signatures in SPI-LCD.h, SSD1963.h,
 * Touch.h. */
void InitDisplaySSD(void) {}
void InitDisplaySPI(int InitOnly) {
    (void)InitOnly;
}
void InitTouch(void) {}

/* SSD1306-over-I²C framebuffer ops. The shared I²C bus driver's
 * InitDisplayI2C wires these to the DrawBuffer/DrawPixel function
 * pointers; VGA-family displays never select an I²C OLED panel so the
 * path is unreached. The real bodies live in spi_lcd.c, which the
 * VGA-family ports do not link, so provide no-op stubs here. */
void DrawRectangleMEM(int x1, int y1, int x2, int y2, int c) {
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    (void)c;
}
void DrawBitmapMEM(int x1, int y1, int width, int height, int scale, int fc, int bc, unsigned char * bitmap) {
    (void)x1;
    (void)y1;
    (void)width;
    (void)height;
    (void)scale;
    (void)fc;
    (void)bc;
    (void)bitmap;
}
void DrawBufferMEM(int x1, int y1, int x2, int y2, unsigned char * p) {
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    (void)p;
}
void ReadBufferMEM(int x1, int y1, int x2, int y2, unsigned char * buff) {
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    (void)buff;
}
void DrawPixelMEM(int x1, int y1, int c) {
    (void)x1;
    (void)y1;
    (void)c;
}

/* TOUCH_*_PIN variables are defined in Touch.c on SPI-LCD ports;
 * on the VGA-family ports Touch.c isn't linked, so satisfy the
 * mmc_stm32 / Editor.c references with zero-initialized stubs. The
 * runtime `if(Option.CombinedCS)` / `if(Option.TOUCH_CS)` guards keep
 * them unreached because the OPTION setter rejects touch
 * configuration on these ports. */
int TOUCH_CS_PIN = 0;
int TOUCH_IRQ_PIN = 0;
int TOUCH_Click_PIN = 0;
