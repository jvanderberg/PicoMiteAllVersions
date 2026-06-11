/*
 * esp32_cyd_vga_mode1.c — MODE 1: 640x480 1-bit graphics on the I2S VGA
 * scanout (matches VGA PicoMite MODE 1's monochrome shape).
 *
 * The framebuffer is bit-packed MSB-first (pixel x = bit 0x80 >> (x & 7)
 * of byte WriteBuf[y * 80 + (x >> 3)]), 38400 bytes. The MMBasic draw
 * hook set below maps any non-black colour to a set bit; the scanout
 * fill ISR expands bits to fg/bg pixel bytes with the shared nibble-mask
 * LUT. Foreground/background RGB222 levels are latched from the console
 * colours when the mode is entered.
 *
 * DrawBuffer/ReadBuffer use the port-wide 3-bytes-per-pixel RGB888
 * contract (PIXEL(), BLT and sprites depend on it): reads return
 * 0xFFFFFF / 0x000000, writes treat any non-black triple as set.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"

#include "esp_attr.h"

#include "vga_i2s_esp32.h"

#define M1_W VGA_I2S_HRES
#define M1_H VGA_I2S_VRES
#define M1_STRIDE (M1_W / 8)

extern unsigned char * WriteBuf;
extern DRAM_ATTR const uint32_t esp32_vga_mask4[16];

/* Scanout colours, latched at mode entry. */
static DRAM_ATTR uint8_t s_m1_fg = 0xFF; /* white */
static DRAM_ATTR uint8_t s_m1_bg = 0xC0; /* black */

void esp32_vga_mode1_set_colours(uint8_t fg_px, uint8_t bg_px) {
    s_m1_fg = fg_px;
    s_m1_bg = bg_px;
}

/* ---- scanout fill (ISR context, core 1) ---- */

void IRAM_ATTR esp32_vga_mode1_fill(int y, uint8_t * dst, void * ctx) {
    (void)ctx;
    const uint8_t * src = (const uint8_t *)WriteBuf + y * M1_STRIDE;
    uint32_t fgw = s_m1_fg * 0x01010101u;
    uint32_t bgw = s_m1_bg * 0x01010101u;
    uint32_t * out = (uint32_t *)dst;
    for (int i = 0; i < M1_STRIDE; i++) {
        unsigned bits = src[i];
        uint32_t m = esp32_vga_mask4[bits >> 4];
        *out++ = (fgw & m) | (bgw & ~m);
        m = esp32_vga_mask4[bits & 0x0F];
        *out++ = (fgw & m) | (bgw & ~m);
    }
}

/* ---- draw primitives (MMBasic hook signatures) ---- */

static inline void m1_set(int x, int y, int on) {
    uint8_t * p = (uint8_t *)WriteBuf + y * M1_STRIDE + (x >> 3);
    uint8_t bit = (uint8_t)(0x80u >> (x & 7));
    if (on)
        *p |= bit;
    else
        *p &= (uint8_t)~bit;
}

static void m1_DrawRectangle(int x1, int y1, int x2, int y2, int c) {
    if (x1 > x2) {
        int t = x1;
        x1 = x2;
        x2 = t;
    }
    if (y1 > y2) {
        int t = y1;
        y1 = y2;
        y2 = t;
    }
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= M1_W) x2 = M1_W - 1;
    if (y2 >= M1_H) y2 = M1_H - 1;
    if (x1 > x2 || y1 > y2) return;
    int on = c != 0;
    for (int y = y1; y <= y2; y++) {
        /* Full bytes in the middle get memset; ragged edges bit-by-bit. */
        int x = x1;
        while (x <= x2 && (x & 7)) m1_set(x++, y, on);
        int full = ((x2 + 1) - x) >> 3;
        if (full > 0) {
            memset((uint8_t *)WriteBuf + y * M1_STRIDE + (x >> 3),
                   on ? 0xFF : 0x00, (size_t)full);
            x += full * 8;
        }
        while (x <= x2) m1_set(x++, y, on);
    }
}

static void m1_DrawBitmap(int x1, int y1, int width, int height, int scale,
                          int fc, int bc, unsigned char * bitmap) {
    int fon = fc != 0;
    int bon = bc != 0;
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < scale; j++) {
            int y = y1 + i * scale + j;
            if (y < 0 || y >= M1_H) continue;
            for (int k = 0; k < width; k++) {
                int lit = (bitmap[((i * width) + k) / 8] >>
                           (((height * width) - ((i * width) + k) - 1) % 8)) &
                          1;
                for (int m = 0; m < scale; m++) {
                    int x = x1 + k * scale + m;
                    if (x < 0 || x >= M1_W) continue;
                    if (lit)
                        m1_set(x, y, fon);
                    else if (bc >= 0)
                        m1_set(x, y, bon);
                }
            }
        }
    }
}

static void m1_DrawBuffer(int x1, int y1, int x2, int y2, unsigned char * p) {
    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            int on = (p[0] | p[1] | p[2]) != 0;
            p += 3;
            if (x >= 0 && x < M1_W && y >= 0 && y < M1_H) m1_set(x, y, on);
        }
    }
}

static void m1_DrawBufferFast(int x1, int y1, int x2, int y2, int blank,
                              unsigned char * p) {
    (void)blank;
    m1_DrawBuffer(x1, y1, x2, y2, p);
}

static void m1_ReadBuffer(int x1, int y1, int x2, int y2, unsigned char * p) {
    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            int on = 0;
            if (x >= 0 && x < M1_W && y >= 0 && y < M1_H)
                on = (WriteBuf[y * M1_STRIDE + (x >> 3)] >> (7 - (x & 7))) & 1;
            uint8_t v = on ? 0xFF : 0x00;
            *p++ = v;
            *p++ = v;
            *p++ = v;
        }
    }
}

static void m1_ReadBufferFast(int x1, int y1, int x2, int y2,
                              unsigned char * p) {
    m1_ReadBuffer(x1, y1, x2, y2, p);
}

static void m1_DrawPixel(int x, int y, int c) {
    if (x < 0 || x >= M1_W || y < 0 || y >= M1_H) return;
    m1_set(x, y, c != 0);
}

static void m1_ScrollLCD(int lines) {
    if (lines == 0) return;
    uint8_t * fb = (uint8_t *)WriteBuf;
    if (lines > 0) {
        if (lines > M1_H) lines = M1_H;
        memmove(fb, fb + lines * M1_STRIDE, (size_t)(M1_H - lines) * M1_STRIDE);
        memset(fb + (M1_H - lines) * M1_STRIDE, 0, (size_t)lines * M1_STRIDE);
    } else {
        lines = -lines;
        if (lines > M1_H) lines = M1_H;
        memmove(fb + lines * M1_STRIDE, fb, (size_t)(M1_H - lines) * M1_STRIDE);
        memset(fb, 0, (size_t)lines * M1_STRIDE);
    }
}

void esp32_vga_mode1_bind_draw(void) {
    DrawRectangle = m1_DrawRectangle;
    DrawBitmap = m1_DrawBitmap;
    ScrollLCD = m1_ScrollLCD;
    DrawBuffer = m1_DrawBuffer;
    ReadBuffer = m1_ReadBuffer;
    DrawBufferFast = m1_DrawBufferFast;
    ReadBufferFast = m1_ReadBufferFast;
    DrawPixel = m1_DrawPixel;
}
