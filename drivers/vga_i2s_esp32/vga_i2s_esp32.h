/*
 * drivers/vga_i2s_esp32/vga_i2s_esp32.h — classic-ESP32 VGA scanout via the
 * I2S1 parallel "LCD mode".
 *
 * Generates a 640x480@60 VGA signal by streaming one byte per pixel clock
 * out of I2S1 with DMA into an external resistor-ladder DAC (e.g. a VGA666
 * board with the two most-significant ladder inputs of each colour wired).
 * Classic ESP32 has no sync generator, so the HSync/VSync levels ride
 * inside every streamed byte:
 *
 *   bit 7 = VSync   bit 6 = HSync   (idle high, pulse low)
 *   bits 5:4 = red  bits 3:2 = green  bits 1:0 = blue   (RGB222)
 *
 * The peripheral loops a descriptor chain of one 800-byte buffer per
 * scanline. Vertical blanking lines share two constant buffers; the 480
 * visible lines cycle through a small ring that a per-line DMA-completion
 * ISR refills via the caller's fill callback, so any presentation model
 * (text cells, packed 1bpp, byte-per-pixel) plugs in above this driver.
 *
 * Technique (registers, clocking, FIFO byte order) follows bitluni's
 * ESP32Lib and the ESP32 TRM; the implementation is original to this
 * project.
 */

#ifndef VGA_I2S_ESP32_H
#define VGA_I2S_ESP32_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VGA_I2S_HRES 640
#define VGA_I2S_VRES 480

/* Both sync bits at idle level; every visible pixel byte must include
 * these or the monitor sees a sync glitch mid-line. */
#define VGA_I2S_SYNC_IDLE 0xC0u

/* Compose a visible pixel byte from 2-bit colour components. */
#define VGA_I2S_RGB222(r, g, b) \
    ((uint8_t)(VGA_I2S_SYNC_IDLE | (((r) & 3u) << 4) | (((g) & 3u) << 2) | ((b) & 3u)))

/* The I2S FIFO in 8-bit LCD mode emits the bytes of each 32-bit word in
 * the order 2,3,0,1: displayed pixel x lives at byte offset x^2 of the
 * line buffer. Fill callbacks can write single pixels with VGA_I2S_PX or
 * pack four consecutive pixels (x % 4 == 0) into one aligned 32-bit word
 * with VGA_I2S_PACK4 — the fast path for expansion loops. */
#define VGA_I2S_PX(line, x, px) ((line)[(x) ^ 2] = (uint8_t)(px))
#define VGA_I2S_PACK4(p0, p1, p2, p3)                           \
    ((uint32_t)(uint8_t)(p2) | ((uint32_t)(uint8_t)(p3) << 8) | \
     ((uint32_t)(uint8_t)(p0) << 16) | ((uint32_t)(uint8_t)(p1) << 24))

/* Fill the 640 visible bytes for scanline y (0..479) into dst. Runs in
 * ISR context on the core that started the driver; keep it short and do
 * not touch flash-resident data while a flash operation may be active.
 * dst is 4-byte aligned; the swizzle above applies. */
typedef void (*vga_i2s_line_fill_fn)(int y, uint8_t * dst, void * ctx);

/* Bring up the scanout on I2S1. data_gpio[i] is the chip GPIO for bus bit
 * i per the byte layout above (data_gpio[6] = HSync, data_gpio[7] =
 * VSync). Allocates ~11 KB of DMA-capable internal RAM (descriptors +
 * line buffers). The fill callback is invoked for every visible line,
 * starting immediately. Returns false on allocation/peripheral failure
 * (nothing is left running). Call from the core that should service the
 * per-line interrupt. */
bool vga_i2s_start(const int8_t data_gpio[8], vga_i2s_line_fill_fn fill, void * ctx);

/* Stop the scanout, release the interrupt and buffers, and detach the
 * pins (left as plain GPIO inputs). Safe to call when not active. */
void vga_i2s_stop(void);

bool vga_i2s_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* VGA_I2S_ESP32_H */
