/*
 * drivers/spi_lcd/spi_lcd_readback.c - portable SPI-LCD panel readback.
 *
 * ReadBufferSPI / ReadBufferSPISCR read pixels back out of the controller's
 * GRAM over MISO (for PIXEL(), sprite save/restore, transparent blits, and
 * readback-based scroll). They were extracted out of spi_lcd.c so every port
 * that implements the hal_spi_lcd_bus contract — Pico shared-bus arbiter or
 * ESP-IDF spi_master — shares one implementation. All panel I/O goes through
 * hal/hal_spi_lcd_bus.h: the read region/command via DefineRegionSPI (which
 * itself uses the contract), the pixel bytes via hal_spi_lcd_bus_read, and
 * the read-clock drop via hal_spi_lcd_bus_read_begin/end.
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "hal/hal_spi_lcd_bus.h"
#include "SPI-LCD.h"

void ReadBufferSPI(int x1, int y1, int x2, int y2, unsigned char * p) {
    int r, N, t;
    unsigned char h, l;
    // make sure the coordinates are kept within the display area
    if (x2 <= x1) {
        t = x1;
        x1 = x2;
        x2 = t;
    }
    if (y2 <= y1) {
        t = y1;
        y1 = y2;
        y2 = t;
    }
    if (x1 < 0) x1 = 0;
    if (x1 >= HRes) x1 = HRes - 1;
    if (x2 < 0) x2 = 0;
    if (x2 >= HRes) x2 = HRes - 1;
    if (y1 < 0) y1 = 0;
    if (y1 >= VRes) y1 = VRes - 1;
    if (y2 < 0) y2 = 0;
    if (y2 >= VRes) y2 = VRes - 1;
    N = (x2 - x1 + 1) * (y2 - y1 + 1) * ((Option.DISPLAY_TYPE == ST7796S || Option.DISPLAY_TYPE == ST7796SP) ? 2 : 3);
    DefineRegionSPI(x1, y1, x2, y2, 0);
    hal_spi_lcd_bus_read_begin(); //need to slow SPI for read on this display
    hal_spi_lcd_bus_read((uint8_t *)p, 1);
    r = 0;
    hal_spi_lcd_bus_read((uint8_t *)p, N);
    hal_spi_lcd_bus_dc(0);
    hal_spi_lcd_bus_end(); //set CS high
    hal_spi_lcd_bus_read_end();
    // revert to non enhanced SPI mode
    r = 0;
    if (Option.DISPLAY_TYPE == ST7796S || Option.DISPLAY_TYPE == ST7796SP) {
        int n = (x2 - x1 + 1) * (y2 - y1 + 1) * 3;
        while (N) {
            h = p[N - 2];
            l = p[N - 1];
            N -= 2;
            p[n - 1] = h & 0xF8;
            p[n - 2] = ((h & 0x7) << 5) | ((l & 0xE0) >> 3);
            p[n - 3] = (l & 0x1F) << 3;
            n -= 3;
        }
    } else {
        while (N) {
            h = (uint8_t)p[r + 2];
            l = (uint8_t)p[r];
            p[r] = (h & 0xFC);
            p[r + 1] &= 0xFC;
            p[r + 2] = (l & 0xFC);
            r += 3;
            N -= 3;
        }
    }
}

void ReadBufferSPISCR(int x1, int y1, int x2, int y2, unsigned char * p) {
    int r, N, t;
    unsigned char h, l;
    // make sure the coordinates are kept within the display area
    if (x2 <= x1) {
        t = x1;
        x1 = x2;
        x2 = t;
    }
    if (y2 <= y1) {
        t = y1;
        y1 = y2;
        y2 = t;
    }
    if (x1 < 0) x1 = 0;
    if (x1 >= HRes) x1 = HRes - 1;
    if (x2 < 0) x2 = 0;
    if (x2 >= HRes) x2 = HRes - 1;
    if (y1 < 0) y1 = 0;
    if (y1 >= VRes) y1 = VRes - 1;
    if (y2 < 0) y2 = 0;
    if (y2 >= VRes) y2 = VRes - 1;
    t = y2 - y1; // get the distance between the top and bottom
    y1 = (y1 + ScrollStart) % VRes;
    y2 = y1 + t;
    if (y2 >= VRes) {
        N = (x2 - x1 + 1) * (y2 - VRes) * ((Option.DISPLAY_TYPE == ST7796S || Option.DISPLAY_TYPE == ST7796SP) ? 2 : 3);
        DefineRegionSPI(x1, y1, x2, VRes - 1, 0);
        hal_spi_lcd_bus_read_begin(); //need to slow SPI for read on this display
        hal_spi_lcd_bus_read((uint8_t *)p, 1);
        r = 0;
        hal_spi_lcd_bus_read((uint8_t *)p, N);
        hal_spi_lcd_bus_dc(0);
        hal_spi_lcd_bus_end(); //set CS high
        hal_spi_lcd_bus_read_end();
        p += N;
        N = (x2 - x1 + 1) * (y2 - VRes) * ((Option.DISPLAY_TYPE == ST7796S || Option.DISPLAY_TYPE == ST7796SP) ? 2 : 3);
        DefineRegionSPI(x1, 0, x2, y2 - VRes, 0);
        hal_spi_lcd_bus_read_begin(); //need to slow SPI for read on this display
        hal_spi_lcd_bus_read((uint8_t *)p, 1);
        r = 0;
        hal_spi_lcd_bus_read((uint8_t *)p, N);
        hal_spi_lcd_bus_dc(0);
        hal_spi_lcd_bus_end(); //set CS high
        hal_spi_lcd_bus_read_end();
        N = (x2 - x1 + 1) * (y2 - y1 + 1) * 3;
    } else {
        N = (x2 - x1 + 1) * (y2 - y1 + 1) * ((Option.DISPLAY_TYPE == ST7796S || Option.DISPLAY_TYPE == ST7796SP) ? 2 : 3);
        DefineRegionSPI(x1, y1, x2, y2, 0);
        hal_spi_lcd_bus_read_begin(); //need to slow SPI for read on this display
        hal_spi_lcd_bus_read((uint8_t *)p, 1);
        r = 0;
        hal_spi_lcd_bus_read((uint8_t *)p, N);
        hal_spi_lcd_bus_dc(0);
        hal_spi_lcd_bus_end(); //set CS high
        hal_spi_lcd_bus_read_end();
        // revert to non enhanced SPI mode
    }
    r = 0;
    if (Option.DISPLAY_TYPE == ST7796S || Option.DISPLAY_TYPE == ST7796SP) {
        N = (x2 - x1 + 1) * (y2 - y1 + 1) * 2;
        int n = (x2 - x1 + 1) * (y2 - y1 + 1) * 3;
        while (N) {
            h = p[N - 2];
            l = p[N - 1];
            N -= 2;
            p[n - 1] = h & 0xF8;
            p[n - 2] = ((h & 0x7) << 5) | ((l & 0xE0) >> 3);
            p[n - 3] = (l & 0x1F) << 3;
            n -= 3;
        }
    } else {
        N = (x2 - x1 + 1) * (y2 - y1 + 1) * 3;
        while (N) {
            h = (uint8_t)p[r + 2];
            l = (uint8_t)p[r];
            p[r] = (h & 0xFC);
            p[r + 1] &= 0xFC;
            p[r + 2] = (l & 0xFC);
            r += 3;
            N -= 3;
        }
    }
}
