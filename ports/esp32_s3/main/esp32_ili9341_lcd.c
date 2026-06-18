/*
 * esp32_ili9341_lcd.c - ESP-IDF SPI-master ILI9341 backend for ESP32-S3.
 *
 * This is intentionally independent of the Pico SPI-LCD driver stack.
 * Option.LCD_* supplies the pins (set by OPTION SYSTEM SPI / OPTION
 * LCDPANEL, or seeded from a board profile); Draw.c supplies the drawing
 * dispatch.
 */

#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "hal/hal_spi_lcd_bus.h"
#include "SPI-LCD.h"
#include "esp32_backlight.h"
#include "esp32_board_profile.h"

#define LCD_W 320
#define LCD_H 240
#define LCD_SPI_HOST SPI3_HOST
#define LCD_SPI_HZ 40000000
#define LCD_DIRTY_TILE 16
#define LCD_DIRTY_COLS ((LCD_W + LCD_DIRTY_TILE - 1) / LCD_DIRTY_TILE)
#define LCD_DIRTY_ROWS ((LCD_H + LCD_DIRTY_TILE - 1) / LCD_DIRTY_TILE)

#define LCD_CASET 0x2A
#define LCD_PASET 0x2B
#define LCD_RAMWR 0x2C
#define LCD_RAMRD 0x2E
/* Panel reads run far slower than writes for solid MISO sampling on the
 * matrix-routed pins (reads are infrequent — PIXEL()/sprites). */
#define LCD_SPI_RD_HZ 2000000

static const char * TAG = "ili9341";
static spi_device_handle_t s_lcd;
/* CS is driven by GPIO (the devices use spics_io_num = -1) so a begin()/end()
 * span can hold it low across a read's command+data sequence. A depth counter
 * keeps writes toggling CS per transaction exactly as hardware CS did. */
static int s_cs_gpio = -1;
static int s_cs_depth;
/* Separate low-clock device for panel RAM reads (RAMRD); shares the bus and
 * the manual CS, so no acquire_bus/CS-keep games (those wedged the writes). */
static spi_device_handle_t s_lcd_rd;
static uint8_t * s_rdbuf;
static int s_rd_n;    /* read index within a read_begin()..read_end() span */
static int s_rd_skip; /* leading bytes to drop from the next data transaction */
static uint32_t * s_shadow;
static int s_dc_gpio = -1;
/* Configured controller (ILI9341 or ST7789B) and the active panel
 * dimensions. The dimensions are the 320x240 landscape binding by default
 * and swap to 240x320 for portrait orientations; the shadow paths are
 * landscape-only (PSRAM boards) and keep using LCD_W/LCD_H directly. */
static int s_panel_type = ILI9341;
static int s_w = LCD_W;
static int s_h = LCD_H;
static uint8_t s_line[LCD_W * 2];
#define LCD_FLUSH_ROWS 16
#define LCD_FLUSH_BYTES (LCD_W * 2 * LCD_FLUSH_ROWS)
static uint8_t * s_flush;
static int s_dirty;
static int s_dirty_x1, s_dirty_y1, s_dirty_x2, s_dirty_y2;
static uint32_t s_dirty_tiles[LCD_DIRTY_ROWS];

extern volatile int DISPLAY_TYPE;
extern unsigned char OptionConsole;
extern const int colours[16];
extern int RGB121map[16];

static uint16_t rgb565(int c) {
    uint32_t v = (uint32_t)c;
    return (uint16_t)(((v & 0x00f80000u) >> 8) |
                      ((v & 0x0000f800u) >> 5) |
                      ((v & 0x000000f8u) >> 3));
}

void esp32_ili9341_lcd_scroll(int lines);
void esp32_ili9341_lcd_flush_pending(void);

static void put565(uint8_t * p, int c) {
    uint16_t v = rgb565(c);
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static int rgb121_palette_colour(uint8_t nibble) {
    nibble &= 0x0f;
    return RGB121map[15] ? RGB121map[nibble] : colours[nibble];
}

static uint8_t packed_rgb121_get(const uint8_t * p, int index) {
    uint8_t b = p[(size_t)index >> 1];
    return (index & 1) ? (uint8_t)(b >> 4) : (uint8_t)(b & 0x0fu);
}

static void packed_rgb121_set(uint8_t * p, int index, uint8_t v) {
    uint8_t * b = &p[(size_t)index >> 1];
    if (index & 1)
        *b = (uint8_t)((*b & 0x0fu) | ((v & 0x0fu) << 4));
    else
        *b = (uint8_t)((*b & 0xf0u) | (v & 0x0fu));
}

/* hal_spi_lcd_bus implementation over ESP-IDF spi_master with GPIO-driven CS.
 * begin()/end() bracket a CS-low span (depth-counted, so nested spans and
 * per-write self-bracketing both work); dc() latches the D/C level; write()
 * goes out the 40 MHz device; read() pulls panel RAM through the low-clock
 * device while the caller holds CS. */
static int s_bus_dc;

void hal_spi_lcd_bus_begin(void) {
    if (s_cs_gpio >= 0 && s_cs_depth++ == 0)
        gpio_set_level((gpio_num_t)s_cs_gpio, 0);
}

void hal_spi_lcd_bus_end(void) {
    if (s_cs_gpio >= 0 && s_cs_depth > 0 && --s_cs_depth == 0)
        gpio_set_level((gpio_num_t)s_cs_gpio, 1);
}

void hal_spi_lcd_bus_dc(int data) {
    s_bus_dc = data ? 1 : 0;
}

static void lcd_tx(const void * data, size_t len, int dc) {
    if (!s_lcd || !data || len == 0) return;
    hal_spi_lcd_bus_begin();
    gpio_set_level((gpio_num_t)s_dc_gpio, dc);
    spi_transaction_t t = {0};
    t.length = len * 8;
    t.tx_buffer = data;
    esp_err_t err = spi_device_polling_transmit(s_lcd, &t);
    if (err != ESP_OK) ESP_LOGW(TAG, "spi transmit failed: %s", esp_err_to_name(err));
    hal_spi_lcd_bus_end();
}

void hal_spi_lcd_bus_write(const uint8_t * buf, size_t len) {
    lcd_tx(buf, len, s_bus_dc);
}

/* Read len bytes of panel RAM. CS is held low by the caller's begin(); the
 * read device runs slow with proper dummy timing. Full-duplex needs
 * length >= rxlength, so we clock zeros out MOSI while sampling MISO, in
 * DMA-scratch-sized chunks. */
/* ESP-IDF re-clocks each SPI transaction independently, so the shared
 * ReadBufferSPI's split into a separate "1 dummy byte" read then "N data
 * bytes" read corrupts the data read's startup. Reconcile by coalescing: the
 * first read after read_begin() is that RAMRD dummy byte — defer it (its value
 * is discarded by the caller) so it rides at the head of the next, single
 * data transaction, which then drops it. The panel emits one dummy byte then
 * R,G,B per pixel (verified by RAMRD raw dump). */
int hal_spi_lcd_bus_read(uint8_t * buf, size_t len) {
    if (!s_lcd_rd || !s_rdbuf || !buf || len == 0) return 0;
    if (++s_rd_n == 1) {
        s_rd_skip = 1; /* defer the dummy to the head of the data transaction */
        return 1;
    }
    gpio_set_level((gpio_num_t)s_dc_gpio, 1);
    const size_t cap = 1 + LCD_W * 3;
    size_t off = 0;
    while (off < len) {
        int skip = s_rd_skip;
        size_t want = len - off;
        if (want > cap - (size_t)skip) want = cap - (size_t)skip;
        size_t total = want + (size_t)skip;
        spi_transaction_t t = {0};
        t.length = total * 8;
        t.rxlength = total * 8;
        t.rx_buffer = s_rdbuf;
        if (spi_device_polling_transmit(s_lcd_rd, &t) != ESP_OK) return 0;
        memcpy(buf + off, s_rdbuf + skip, want);
        off += want;
        s_rd_skip = 0; /* dummy consumed; continuation chunks have none */
    }
    return 1;
}

/* Reads run on a dedicated low-clock device, so there's no clock to switch;
 * just reset the per-span read index so the first read consumes the RAMRD
 * dummy byte. */
void hal_spi_lcd_bus_read_begin(void) {
    s_rd_n = 0;
}
void hal_spi_lcd_bus_read_end(void) {}

static void lcd_cmd(uint8_t cmd) {
    hal_spi_lcd_bus_dc(0);
    hal_spi_lcd_bus_write(&cmd, 1);
}

static void lcd_data(const uint8_t * data, size_t len) {
    hal_spi_lcd_bus_dc(1);
    hal_spi_lcd_bus_write(data, len);
}

static void lcd_cmd_data(uint8_t cmd, const uint8_t * data, size_t len) {
    lcd_cmd(cmd);
    lcd_data(data, len);
}

static void lcd_addr_window(int x1, int y1, int x2, int y2) {
    uint8_t b[4];
    b[0] = (uint8_t)(x1 >> 8);
    b[1] = (uint8_t)x1;
    b[2] = (uint8_t)(x2 >> 8);
    b[3] = (uint8_t)x2;
    lcd_cmd_data(LCD_CASET, b, sizeof(b));
    b[0] = (uint8_t)(y1 >> 8);
    b[1] = (uint8_t)y1;
    b[2] = (uint8_t)(y2 >> 8);
    b[3] = (uint8_t)y2;
    lcd_cmd_data(LCD_PASET, b, sizeof(b));
    lcd_cmd(LCD_RAMWR);
}

static void lcd_push_line_rgb565(int x, int y, int w, const uint8_t * line) {
    if (w <= 0) return;
    lcd_addr_window(x, y, x + w - 1, y);
    lcd_data(line, (size_t)w * 2u);
}

static uint8_t * lcd_batch_buffer(int w, int * rows_per_batch) {
    int rows = 1;
    uint8_t * buf = s_line;
    if (s_flush && w > 0) {
        rows = LCD_FLUSH_BYTES / (w * 2);
        if (rows < 1) rows = 1;
        if (rows > LCD_FLUSH_ROWS) rows = LCD_FLUSH_ROWS;
        buf = s_flush;
    }
    if (rows_per_batch) *rows_per_batch = rows;
    return buf;
}

static uint8_t * lcd_batch_buffer_for_area(int w, int h, int * rows_per_batch) {
    if (!s_flush || w <= 0 || h <= 0 || (size_t)w * (size_t)h * 2u < 2048u) {
        if (rows_per_batch) *rows_per_batch = 1;
        return s_line;
    }
    int rows;
    uint8_t * buf = lcd_batch_buffer(w, &rows);
    if (rows > h) rows = h;
    if (rows_per_batch) *rows_per_batch = rows;
    return buf;
}

static int clip_rect(int * x1, int * y1, int * x2, int * y2) {
    if (*x1 > *x2) {
        int t = *x1;
        *x1 = *x2;
        *x2 = t;
    }
    if (*y1 > *y2) {
        int t = *y1;
        *y1 = *y2;
        *y2 = t;
    }
    if (*x1 < 0) *x1 = 0;
    if (*y1 < 0) *y1 = 0;
    if (*x2 >= s_w) *x2 = s_w - 1;
    if (*y2 >= s_h) *y2 = s_h - 1;
    return *x1 <= *x2 && *y1 <= *y2;
}

static void shadow_rect(int x1, int y1, int x2, int y2, uint32_t c) {
    if (!s_shadow) return;
    for (int y = y1; y <= y2; y++) {
        uint32_t * row = s_shadow + (size_t)y * LCD_W;
        for (int x = x1; x <= x2; x++) row[x] = c;
    }
}

static void dirty_mark(int x1, int y1, int x2, int y2) {
    if (!s_shadow || !clip_rect(&x1, &y1, &x2, &y2)) return;
    int tx1 = x1 / LCD_DIRTY_TILE;
    int tx2 = x2 / LCD_DIRTY_TILE;
    int ty1 = y1 / LCD_DIRTY_TILE;
    int ty2 = y2 / LCD_DIRTY_TILE;
    uint32_t bits = ((1u << (tx2 - tx1 + 1)) - 1u) << tx1;
    for (int ty = ty1; ty <= ty2; ty++) s_dirty_tiles[ty] |= bits;
    if (!s_dirty) {
        s_dirty_x1 = x1;
        s_dirty_y1 = y1;
        s_dirty_x2 = x2;
        s_dirty_y2 = y2;
    } else {
        if (x1 < s_dirty_x1) s_dirty_x1 = x1;
        if (y1 < s_dirty_y1) s_dirty_y1 = y1;
        if (x2 > s_dirty_x2) s_dirty_x2 = x2;
        if (y2 > s_dirty_y2) s_dirty_y2 = y2;
    }
    s_dirty = 1;
}

static void dirty_clear(void) {
    s_dirty = 0;
    s_dirty_x1 = s_dirty_y1 = 0;
    s_dirty_x2 = s_dirty_y2 = 0;
    memset(s_dirty_tiles, 0, sizeof(s_dirty_tiles));
}

static void flush_shadow_rect(int x1, int y1, int x2, int y2) {
    if (!s_shadow || !clip_rect(&x1, &y1, &x2, &y2)) return;
    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    int rows_per_batch;
    uint8_t * tx = lcd_batch_buffer(w, &rows_per_batch);

    lcd_addr_window(x1, y1, x2, y2);
    for (int yoff = 0; yoff < h;) {
        int rows = h - yoff;
        if (rows > rows_per_batch) rows = rows_per_batch;
        uint8_t * out = tx;
        for (int ry = 0; ry < rows; ry++) {
            uint32_t * src = s_shadow + (size_t)(y1 + yoff + ry) * LCD_W + x1;
            for (int x = 0; x < w; x++) {
                put565(out, (int)src[x]);
                out += 2;
            }
        }
        lcd_data(tx, (size_t)rows * (size_t)w * 2u);
        yoff += rows;
    }
}

static void esp32_lcd_draw_rectangle(int x1, int y1, int x2, int y2, int c) {
    if (!clip_rect(&x1, &y1, &x2, &y2)) return;
    esp32_ili9341_lcd_flush_pending();
    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    uint32_t c24 = (uint32_t)c & 0x00ffffffu;
    shadow_rect(x1, y1, x2, y2, c24);

    int rows_per_batch;
    uint8_t * tx = lcd_batch_buffer_for_area(w, h, &rows_per_batch);
    uint8_t * row0 = tx;
    for (int x = 0; x < w; x++) put565(&row0[x * 2], c);
    for (int y = 1; y < rows_per_batch; y++)
        memcpy(tx + (size_t)y * w * 2u, row0, (size_t)w * 2u);

    lcd_addr_window(x1, y1, x2, y2);
    for (int y = 0; y < h;) {
        int rows = h - y;
        if (rows > rows_per_batch) rows = rows_per_batch;
        lcd_data(tx, (size_t)rows * w * 2u);
        y += rows;
    }
}

static void esp32_lcd_draw_pixel(int x, int y, int c) {
    if (x < 0 || y < 0 || x >= s_w || y >= s_h) return;
    if (s_shadow) {
        s_shadow[(size_t)y * LCD_W + (size_t)x] = (uint32_t)c & 0x00ffffffu;
        dirty_mark(x, y, x, y);
        return;
    }
    uint8_t px[2];
    put565(px, c);
    lcd_addr_window(x, y, x, y);
    lcd_data(px, sizeof(px));
}

/* Direct-to-panel glyph/bitmap render for no-shadow boards. Each output
 * row is built into s_line as RGB565 and pushed as a one-row window:
 * opaque (bc >= 0) draws the whole clipped row in one burst; transparent
 * (bc < 0) pushes only the foreground runs, leaving the background
 * untouched. */
static void esp32_lcd_draw_bitmap_direct(int x, int y, int width, int height,
                                         int scale, int fc, int bc,
                                         unsigned char * bitmap) {
    int total_bits = width * height;
    int out_w = width * scale;
    int out_h = height * scale;
    int cx1 = x, cy1 = y, cx2 = x + out_w - 1, cy2 = y + out_h - 1;
    if (!clip_rect(&cx1, &cy1, &cx2, &cy2)) return;
    for (int py = cy1; py <= cy2; py++) {
        int row = (py - y) / scale;
        int run_x = -1, run_w = 0;
        for (int px = cx1; px <= cx2; px++) {
            int col = (px - x) / scale;
            int bit = row * width + col;
            int on = (bitmap[bit / 8] >> ((total_bits - bit - 1) % 8)) & 1;
            if (!on && bc < 0) {
                if (run_w) {
                    lcd_push_line_rgb565(run_x, py, run_w, s_line);
                    run_w = 0;
                    run_x = -1;
                }
                continue;
            }
            int c = on ? fc : bc;
            if (run_x < 0) run_x = px;
            put565(&s_line[run_w * 2], c);
            run_w++;
        }
        if (run_w) lcd_push_line_rgb565(run_x, py, run_w, s_line);
    }
}

static void esp32_lcd_draw_bitmap(int x, int y, int width, int height,
                                  int scale, int fc, int bc,
                                  unsigned char * bitmap) {
    if (!bitmap || width <= 0 || height <= 0) return;
    if (scale < 1) scale = 1;
    if (!s_shadow) {
        esp32_ili9341_lcd_flush_pending();
        esp32_lcd_draw_bitmap_direct(x, y, width, height, scale, fc, bc, bitmap);
        return;
    }
    int total_bits = width * height;
    int out_w = width * scale;
    int out_h = height * scale;
    int cx1 = x, cy1 = y, cx2 = x + out_w - 1, cy2 = y + out_h - 1;
    if (!clip_rect(&cx1, &cy1, &cx2, &cy2)) return;
    int dirty = bc >= 0;

    for (int py = cy1; py <= cy2; py++) {
        int row = (py - y) / scale;
        for (int px = cx1; px <= cx2; px++) {
            int col = (px - x) / scale;
            int bit = row * width + col;
            int on = (bitmap[bit / 8] >> ((total_bits - bit - 1) % 8)) & 1;
            if (!on && bc < 0) continue;
            int c = on ? fc : bc;
            if (s_shadow)
                s_shadow[(size_t)py * LCD_W + (size_t)px] = (uint32_t)c & 0x00ffffffu;
            dirty = 1;
        }
    }
    if (dirty) dirty_mark(cx1, cy1, cx2, cy2);
}

static void esp32_lcd_draw_buffer(int x1, int y1, int x2, int y2,
                                  unsigned char * bgr) {
    if (!bgr) return;
    esp32_ili9341_lcd_flush_pending();
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
    int src_w = x2 - x1 + 1;
    int cx1 = x1, cy1 = y1, cx2 = x2, cy2 = y2;
    if (!clip_rect(&cx1, &cy1, &cx2, &cy2)) return;
    int w = cx2 - cx1 + 1;
    int h = cy2 - cy1 + 1;
    int rows_per_batch;
    uint8_t * tx = lcd_batch_buffer_for_area(w, h, &rows_per_batch);
    lcd_addr_window(cx1, cy1, cx2, cy2);

    for (int yoff = 0; yoff < h;) {
        int rows = h - yoff;
        if (rows > rows_per_batch) rows = rows_per_batch;
        for (int ry = 0; ry < rows; ry++) {
            int y = cy1 + yoff + ry;
            unsigned char * src = bgr + ((size_t)(y - y1) * (size_t)src_w +
                                         (size_t)(cx1 - x1)) *
                                            3u;
            uint8_t * out = tx + (size_t)ry * w * 2u;
            for (int x = cx1; x <= cx2; x++) {
                int c = ((int)src[2] << 16) | ((int)src[1] << 8) | src[0];
                if (s_shadow) s_shadow[(size_t)y * LCD_W + (size_t)x] = (uint32_t)c;
                put565(&out[(x - cx1) * 2], c);
                src += 3;
            }
        }
        lcd_data(tx, (size_t)rows * w * 2u);
        yoff += rows;
    }
}

static void esp32_lcd_draw_buffer_fast(int x1, int y1, int x2, int y2,
                                       int blank, unsigned char * p) {
    if (!p) return;
    esp32_ili9341_lcd_flush_pending();
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
    int src_w = x2 - x1 + 1;
    int cx1 = x1, cy1 = y1, cx2 = x2, cy2 = y2;
    if (!clip_rect(&cx1, &cy1, &cx2, &cy2)) return;
    for (int y = cy1; y <= cy2; y++) {
        int run_x = -1;
        int run_w = 0;
        for (int x = cx1; x <= cx2; x++) {
            int src_index = (y - y1) * src_w + (x - x1);
            uint8_t nibble = packed_rgb121_get(p, src_index);
            if (blank != -1 && nibble == sprite_transparent) {
                if (run_w) {
                    lcd_push_line_rgb565(run_x, y, run_w, s_line);
                    run_w = 0;
                    run_x = -1;
                }
                continue;
            }
            int c = rgb121_palette_colour(nibble);
            if (run_x < 0) run_x = x;
            if (run_x + run_w != x) {
                lcd_push_line_rgb565(run_x, y, run_w, s_line);
                run_x = x;
                run_w = 0;
            }
            if (s_shadow) s_shadow[(size_t)y * LCD_W + (size_t)x] = (uint32_t)c;
            put565(&s_line[run_w * 2], c);
            run_w++;
        }
        if (run_w) lcd_push_line_rgb565(run_x, y, run_w, s_line);
    }
}

static void esp32_lcd_read_buffer(int x1, int y1, int x2, int y2,
                                  unsigned char * bgr) {
    if (!bgr) return;
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
    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            uint32_t c = 0;
            if (s_shadow && x >= 0 && y >= 0 && x < LCD_W && y < LCD_H)
                c = s_shadow[(size_t)y * LCD_W + (size_t)x];
            *bgr++ = (uint8_t)c;
            *bgr++ = (uint8_t)(c >> 8);
            *bgr++ = (uint8_t)(c >> 16);
        }
    }
}

static void esp32_lcd_read_buffer_fast(int x1, int y1, int x2, int y2,
                                       unsigned char * p) {
    if (!p) return;
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
    int index = 0;
    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            uint8_t nibble = 0;
            if (s_shadow && x >= 0 && y >= 0 && x < LCD_W && y < LCD_H)
                nibble = RGB121(s_shadow[(size_t)y * LCD_W + (size_t)x]);
            packed_rgb121_set(p, index++, nibble);
        }
    }
}

static void flush_shadow_full(void) {
    if (!s_shadow) return;
    flush_shadow_rect(0, 0, LCD_W - 1, LCD_H - 1);
    dirty_clear();
}

void esp32_ili9341_lcd_flush_pending(void) {
    if (!s_dirty || !s_shadow || !s_lcd) return;
    uint32_t dirty[LCD_DIRTY_ROWS];
    memcpy(dirty, s_dirty_tiles, sizeof(dirty));
    int x1 = s_dirty_x1;
    int y1 = s_dirty_y1;
    int x2 = s_dirty_x2;
    int y2 = s_dirty_y2;
    dirty_clear();

    int bbox_area = (x2 - x1 + 1) * (y2 - y1 + 1);
    int tile_area = 0;
    for (int ty = 0; ty < LCD_DIRTY_ROWS; ty++) {
        uint32_t mask = dirty[ty];
        while (mask) {
            int tx = __builtin_ctz(mask);
            int tw = LCD_DIRTY_TILE;
            int th = LCD_DIRTY_TILE;
            if ((tx + 1) * LCD_DIRTY_TILE > LCD_W) tw = LCD_W - tx * LCD_DIRTY_TILE;
            if ((ty + 1) * LCD_DIRTY_TILE > LCD_H) th = LCD_H - ty * LCD_DIRTY_TILE;
            tile_area += tw * th;
            mask &= ~(1u << tx);
        }
    }
    if (bbox_area <= tile_area) {
        flush_shadow_rect(x1, y1, x2, y2);
        return;
    }

    for (int ty = 0; ty < LCD_DIRTY_ROWS;) {
        if (!dirty[ty]) {
            ty++;
            continue;
        }
        uint32_t mask = dirty[ty];
        int tx1 = __builtin_ctz(mask);
        int tx2 = tx1;
        while (tx2 + 1 < LCD_DIRTY_COLS && (mask & (1u << (tx2 + 1)))) tx2++;
        uint32_t span = ((1u << (tx2 - tx1 + 1)) - 1u) << tx1;
        int ty2 = ty;
        while (ty2 + 1 < LCD_DIRTY_ROWS && dirty[ty2 + 1] == span) ty2++;

        int x1 = tx1 * LCD_DIRTY_TILE;
        int y1 = ty * LCD_DIRTY_TILE;
        int x2 = (tx2 + 1) * LCD_DIRTY_TILE - 1;
        int y2 = (ty2 + 1) * LCD_DIRTY_TILE - 1;
        flush_shadow_rect(x1, y1, x2, y2);

        for (int row = ty; row <= ty2; row++) dirty[row] &= ~span;
        if (!dirty[ty]) ty++;
    }
}

int esp32_ili9341_lcd_ready(void) {
    return s_lcd != NULL && s_shadow != NULL && Option.DISPLAY_TYPE == s_panel_type &&
           HRes == s_w && VRes == s_h;
}

/* The S3/Freenove backend keeps a PSRAM shadow that backs an off-screen
 * FRAMEBUFFER whenever the panel is ready. */
int esp32_ili9341_lcd_framebuffer_supported(void) {
    return esp32_ili9341_lcd_ready();
}

static void esp32_ili9341_lcd_bind_panel(void) {
    HRes = DisplayHRes = s_w;
    VRes = DisplayVRes = s_h;
    ScreenSize = s_w * s_h;
    Option.DISPLAY_TYPE = s_panel_type;
    DISPLAY_TYPE = s_panel_type;
    Option.DISPLAY_CONSOLE = 1;
    Option.Refresh = 0;
    OptionConsole = 3;
    gui_colour_depth = 16; /* RGB565 */
    DrawPixel = esp32_lcd_draw_pixel;
    DrawRectangle = esp32_lcd_draw_rectangle;
    DrawBitmap = esp32_lcd_draw_bitmap;
    DrawBuffer = esp32_lcd_draw_buffer;
    DrawBLITBuffer = esp32_lcd_draw_buffer;
    DrawBufferFast = esp32_lcd_draw_buffer_fast;
    ScrollLCD = ScrollLCDSPISCR;
    if (s_shadow) {
        /* PSRAM shadow is the readback source (S3/Freenove). */
        ReadBuffer = esp32_lcd_read_buffer;
        ReadBLITBuffer = esp32_lcd_read_buffer;
        ReadBufferFast = esp32_lcd_read_buffer_fast;
        Option.NoScroll = 0;
    } else if (s_lcd_rd && s_rdbuf) {
        /* No shadow, but the panel returns RAM over MISO: read pixels back
         * through the shared SPI-LCD readback (drivers/spi_lcd/
         * spi_lcd_readback.c) over this driver's hal_spi_lcd_bus. PIXEL() and
         * transparent blits work; a fast RGB121 reader and readback scroll
         * are still to come, so the console clear-homes for now. */
        ReadBuffer = ReadBufferSPI;
        ReadBLITBuffer = ReadBufferSPI;
        ReadBufferFast = (void (*)(int, int, int, int, unsigned char *))DisplayNotSet;
        Option.NoScroll = 1;
    } else {
        ReadBuffer = (void (*)(int, int, int, int, unsigned char *))DisplayNotSet;
        ReadBLITBuffer = (void (*)(int, int, int, int, unsigned char *))DisplayNotSet;
        ReadBufferFast = (void (*)(int, int, int, int, unsigned char *))DisplayNotSet;
        Option.NoScroll = 1;
    }
}

int esp32_ili9341_lcd_restore_panel(void) {
    if (!s_lcd) return 0;
    esp32_ili9341_lcd_bind_panel();
    return 1;
}

int esp32_ili9341_lcd_fastgfx_ready(void) {
    return esp32_ili9341_lcd_ready() && PSRAMsize != 0;
}

void esp32_ili9341_lcd_snapshot_rgb121(uint8_t * out) {
    if (!out || !esp32_ili9341_lcd_ready()) return;
    int index = 0;
    for (int y = 0; y < LCD_H; y++) {
        for (int x = 0; x < LCD_W; x++) {
            uint8_t nibble = RGB121(s_shadow[(size_t)y * LCD_W + (size_t)x]);
            packed_rgb121_set(out, index++, nibble);
        }
    }
}

void esp32_ili9341_lcd_present_rgb121_diff(uint8_t * back, uint8_t * front) {
    if (!esp32_ili9341_lcd_ready() || !back || !front) return;
    esp32_ili9341_lcd_flush_pending();
    const int stride = LCD_W / 2;
    int y = 0;
    while (y < LCD_H) {
        if (memcmp(back + (size_t)y * stride, front + (size_t)y * stride,
                   (size_t)stride) == 0) {
            y++;
            continue;
        }

        int y_start = y;
        int x_min = stride;
        int x_max = 0;
        while (y < LCD_H &&
               memcmp(back + (size_t)y * stride, front + (size_t)y * stride,
                      (size_t)stride) != 0) {
            uint8_t * b = back + (size_t)y * stride;
            uint8_t * f = front + (size_t)y * stride;
            for (int x = 0; x < stride; x++) {
                if (b[x] != f[x]) {
                    if (x < x_min) x_min = x;
                    if (x > x_max) x_max = x;
                }
            }
            y++;
        }
        int y_end = y;
        int px_start = x_min * 2;
        int px_end = x_max * 2 + 1;
        int pixel_count = px_end - px_start + 1;
        int rows_per_batch;
        uint8_t * tx = lcd_batch_buffer(pixel_count, &rows_per_batch);

        for (int sy = y_start; sy < y_end;) {
            int rows = y_end - sy;
            if (rows > rows_per_batch) rows = rows_per_batch;
            for (int ry = 0; ry < rows; ry++) {
                int yrow = sy + ry;
                uint8_t * src = back + (size_t)yrow * stride + x_min;
                uint8_t * dst = front + (size_t)yrow * stride + x_min;
                uint8_t * outp = tx + (size_t)ry * pixel_count * 2u;
                for (int bx = x_min, out = 0; bx <= x_max; bx++) {
                    uint8_t byte = src[bx - x_min];
                    int c0 = rgb121_palette_colour(byte);
                    int c1 = rgb121_palette_colour(byte >> 4);
                    if (s_shadow) {
                        size_t off = (size_t)yrow * LCD_W + (size_t)(bx * 2);
                        s_shadow[off] = (uint32_t)c0 & 0x00ffffffu;
                        s_shadow[off + 1] = (uint32_t)c1 & 0x00ffffffu;
                    }
                    put565(&outp[out], c0);
                    out += 2;
                    put565(&outp[out], c1);
                    out += 2;
                }
                memcpy(dst, src, (size_t)(x_max - x_min + 1));
            }
            lcd_addr_window(px_start, sy, px_end, sy + rows - 1);
            lcd_data(tx, (size_t)rows * pixel_count * 2u);
            sy += rows;
        }
    }
}

void esp32_ili9341_lcd_present_rgb121_rect(const uint8_t * src,
                                           int xstart, int xend,
                                           int ystart, int yend,
                                           int odd) {
    if (!esp32_ili9341_lcd_ready() || !src) return;
    esp32_ili9341_lcd_flush_pending();
    int src_w = xend - xstart + 1;
    if (src_w <= 0) return;
    int cx1 = xstart, cy1 = ystart, cx2 = xend, cy2 = yend;
    if (!clip_rect(&cx1, &cy1, &cx2, &cy2)) return;
    int w = cx2 - cx1 + 1;
    int h = cy2 - cy1 + 1;
    int rows_per_batch;
    uint8_t * tx = lcd_batch_buffer(w, &rows_per_batch);

    lcd_addr_window(cx1, cy1, cx2, cy2);
    for (int yoff = 0; yoff < h;) {
        int rows = h - yoff;
        if (rows > rows_per_batch) rows = rows_per_batch;
        for (int ry = 0; ry < rows; ry++) {
            int y = cy1 + yoff + ry;
            int src_row_base = (y - ystart) * src_w;
            uint8_t * out = tx + (size_t)ry * w * 2u;
            for (int x = cx1; x <= cx2; x++) {
                int src_index = src_row_base + (x - xstart);
                int packed_index = src_index + (odd ? 1 : 0);
                uint8_t byte = src[(size_t)packed_index >> 1];
                uint8_t nibble = (packed_index & 1) ? (uint8_t)(byte >> 4)
                                                    : (uint8_t)(byte & 0x0f);
                int c = rgb121_palette_colour(nibble);
                if (s_shadow)
                    s_shadow[(size_t)y * LCD_W + (size_t)x] = (uint32_t)c & 0x00ffffffu;
                put565(&out[(x - cx1) * 2], c);
            }
        }
        lcd_data(tx, (size_t)rows * w * 2u);
        yoff += rows;
    }
}

void esp32_ili9341_lcd_scroll(int lines) {
    if (!s_shadow || lines == 0) return;
    ScrollStart = 0;
    uint32_t bg = (uint32_t)gui_bcolour & 0x00ffffffu;
    if (lines <= -LCD_H || lines >= LCD_H) {
        for (size_t i = 0; i < (size_t)LCD_W * LCD_H; i++) s_shadow[i] = bg;
        flush_shadow_full();
        return;
    }
    int abs_lines = lines < 0 ? -lines : lines;
    if (lines > 0) {
        size_t row_words = LCD_W;
        memmove(s_shadow, s_shadow + (size_t)lines * row_words,
                (size_t)(LCD_H - lines) * row_words * sizeof(*s_shadow));
        uint32_t * tail = s_shadow + (size_t)(LCD_H - lines) * row_words;
        for (size_t i = 0; i < (size_t)lines * row_words; i++) tail[i] = bg;
    } else {
        size_t row_words = LCD_W;
        memmove(s_shadow + (size_t)abs_lines * row_words, s_shadow,
                (size_t)(LCD_H - abs_lines) * row_words * sizeof(*s_shadow));
        for (size_t i = 0; i < (size_t)abs_lines * row_words; i++) s_shadow[i] = bg;
    }
    flush_shadow_full();
}

static void lcd_init_controller(void) {
    /* Run the shared SPI LCD controller init (drivers/spi_lcd/
     * spi_lcd_panels.c) through this file's hal_spi_lcd_bus entry points.
     * The panel core keys on Option.DISPLAY_TYPE and Option.DISPLAY_ORIENTATION
     * (which selects the MADCTL rotation); the configured pins gate entry to
     * this driver, so bind the type before initialising. Panel colour-polarity
     * quirks come from Option.BGR (INVON on ILI9341, INVOFF on ST7789). */
    Option.DISPLAY_TYPE = s_panel_type;
    DISPLAY_TYPE = s_panel_type;
    (void)spi_lcd_panel_init();
    ScrollStart = 0;
}

/* Diagnostic: read the controller's ID register (RDDID, 0x04) over MISO.
 * A non-zero ID means the panel actually drives SDO and full panel readback
 * (sprites, PIXEL(), scroll) is achievable; all-zero means SDO isn't wired/
 * driven on this board and readback must fall back. */
static void panel_miso_diag(void) {
    if (!s_lcd_rd || !s_rdbuf) return;
    /* Write a pixel with distinct R/G/B, then dump the raw RAMRD bytes so the
     * read alignment is visible (R=FC, G=80, B=40). */
    uint8_t px[2];
    put565(px, 0x00FC8040);
    lcd_addr_window(0, 0, 0, 0);
    lcd_data(px, sizeof(px));
    if (Option.DISPLAY_TYPE == ST7789B || Option.DISPLAY_TYPE == ILI9341)
        spi_write_cd(ILI9341_PIXELFORMAT, 1, 0x66);
    DefineRegionSPI(0, 0, 0, 0, 0); /* CASET/PASET/RAMRD, CS held */
    gpio_set_level((gpio_num_t)s_dc_gpio, 1);
    spi_transaction_t t = {0};
    t.length = 12 * 8;
    t.rxlength = 12 * 8;
    t.rx_buffer = s_rdbuf;
    spi_device_polling_transmit(s_lcd_rd, &t);
    hal_spi_lcd_bus_dc(0);
    hal_spi_lcd_bus_end();
    if (Option.DISPLAY_TYPE == ST7789B || Option.DISPLAY_TYPE == ILI9341)
        spi_write_cd(ILI9341_PIXELFORMAT, 1, 0x55);
    ESP_LOGI(TAG,
             "RAMRD raw (wrote R=FC G=80 B=40): %02x %02x %02x %02x %02x %02x "
             "%02x %02x %02x %02x",
             s_rdbuf[0], s_rdbuf[1], s_rdbuf[2], s_rdbuf[3], s_rdbuf[4],
             s_rdbuf[5], s_rdbuf[6], s_rdbuf[7], s_rdbuf[8], s_rdbuf[9]);
}

/* Display pins live in Option.LCD_* / Option.DISPLAY_BL as pin indices —
 * set by OPTION SYSTEM SPI / OPTION LCDPANEL, or seeded from a board
 * profile's defaults. Resolve an index to the raw GPIO the ESP-IDF
 * SPI/GPIO drivers need; 0 means not configured. */
static int lcd_option_gpio(int pin) {
    if (pin <= 0 || pin > NBRPINS || (PinDef[pin].mode & UNUSED)) return -1;
    return PinDef[pin].GPno;
}

static int lcd_pin_available(int pin) {
    if (pin == 0) return 1; /* optional pin left unconfigured */
    if (pin < 0 || pin > NBRPINS) return 0;
    return ExtCurrentConfig[pin] == EXT_NOT_CONFIG;
}

static int lcd_option_pins_available(void) {
    return lcd_pin_available(Option.LCD_CLK) &&
           lcd_pin_available(Option.LCD_MOSI) &&
           lcd_pin_available(Option.LCD_MISO) &&
           lcd_pin_available(Option.LCD_CS) &&
           lcd_pin_available(Option.LCD_CD) &&
           lcd_pin_available(Option.LCD_Reset) &&
           lcd_pin_available(Option.DISPLAY_BL);
}

static void lcd_release_resources(int bus_inited) {
    dirty_clear();
    if (s_lcd_rd) {
        (void)spi_bus_remove_device(s_lcd_rd);
        s_lcd_rd = NULL;
    }
    if (s_lcd) {
        (void)spi_bus_remove_device(s_lcd);
        s_lcd = NULL;
    }
    if (bus_inited) (void)spi_bus_free(LCD_SPI_HOST);
    if (s_rdbuf) {
        heap_caps_free(s_rdbuf);
        s_rdbuf = NULL;
    }
    s_cs_gpio = -1;
    s_cs_depth = 0;
    if (s_shadow) {
        heap_caps_free(s_shadow);
        s_shadow = NULL;
    }
    if (s_flush) {
        heap_caps_free(s_flush);
        s_flush = NULL;
    }
    s_dc_gpio = -1;
}

void esp32_ili9341_lcd_init(void) {
    /* The configured control pins are the panel selection: OPTION LCDPANEL
     * sets them, OPTION LCDPANEL DISABLE clears them. Option.DISPLAY_TYPE is
     * bound state (bind_panel rewrites it) and the web console clobbers it,
     * so it is deliberately not part of this gate. */
    if (Option.WebConsole || s_lcd) return;
    if (!Option.LCD_CD || !Option.LCD_CS) return; /* no panel configured */
    dirty_clear();
    const int sclk = lcd_option_gpio(Option.LCD_CLK);
    const int mosi = lcd_option_gpio(Option.LCD_MOSI);
    const int miso = lcd_option_gpio(Option.LCD_MISO);
    const int cs = lcd_option_gpio(Option.LCD_CS);
    const int dc = lcd_option_gpio(Option.LCD_CD);
    const int rst = lcd_option_gpio(Option.LCD_Reset);
    const int backlight = lcd_option_gpio(Option.DISPLAY_BL);
    if (sclk < 0 || mosi < 0 || cs < 0 || dc < 0) {
        ESP_LOGW(TAG, "LCD pins incomplete; set OPTION SYSTEM SPI and "
                      "OPTION LCDPANEL");
        return;
    }
    if (!lcd_option_pins_available()) {
        ESP_LOGW(TAG, "configured LCD pins are already in use");
        return;
    }

    spi_bus_config_t bus = {
        .mosi_io_num = mosi,
        .miso_io_num = miso,
        .sclk_io_num = sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_FLUSH_BYTES,
    };
    esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return;
    }

    /* CS is driven by GPIO (spics_io_num = -1) so reads can hold it low
     * across the RAMRD command + data. The write device runs at 40 MHz with
     * NO_DUMMY (the only-writes clock-cap bypass for matrix-routed pins);
     * the read device runs slow with default dummy timing. */
    spi_device_interface_config_t dev = {
        .clock_speed_hz = LCD_SPI_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
        .flags = SPI_DEVICE_NO_DUMMY,
    };
    err = spi_bus_add_device(LCD_SPI_HOST, &dev, &s_lcd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        lcd_release_resources(1);
        return;
    }
    spi_device_interface_config_t rdev = {
        .clock_speed_hz = LCD_SPI_RD_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
        /* MISO round-trips through the GPIO matrix + panel output delay;
         * tell the driver so it samples in the valid window. */
        .input_delay_ns = 80,
    };
    if (spi_bus_add_device(LCD_SPI_HOST, &rdev, &s_lcd_rd) != ESP_OK) {
        ESP_LOGW(TAG, "read device add failed; panel readback disabled");
        s_lcd_rd = NULL;
    }
    if (!s_rdbuf)
        s_rdbuf = heap_caps_malloc(1 + LCD_W * 3, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    int actual_khz = 0;
    err = spi_device_get_actual_freq(s_lcd, &actual_khz);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "LCD SPI configured=%d Hz actual=%d kHz",
                 dev.clock_speed_hz, actual_khz);
    } else {
        ESP_LOGW(TAG, "spi_device_get_actual_freq failed: %s", esp_err_to_name(err));
    }

    s_dc_gpio = dc;
    s_cs_gpio = cs;
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << (uint32_t)s_dc_gpio) | (1ULL << (uint32_t)cs),
        .mode = GPIO_MODE_OUTPUT,
    };
    if (backlight >= 0)
        out.pin_bit_mask |= 1ULL << (uint32_t)backlight;
    if (rst >= 0)
        out.pin_bit_mask |= 1ULL << (uint32_t)rst;
    err = gpio_config(&out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        lcd_release_resources(1);
        return;
    }
    gpio_set_level((gpio_num_t)cs, 1); /* CS idle high */

    if (rst >= 0) {
        gpio_set_level((gpio_num_t)rst, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level((gpio_num_t)rst, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    if (backlight >= 0)
        esp32_backlight_init_default();

    /* PSRAM-backed boards (S3/Freenove) keep a full 320x240x4 shadow as the
     * source of truth for readback, dirty-rect flush, scroll, and FASTGFX.
     * No-PSRAM boards (classic-ESP32 CYD) have no room for it and run a
     * direct-to-panel path instead: drawing writes straight to the
     * controller and the shadow stays NULL. PSRAMsize is the shared,
     * published signal for "external PSRAM exists" (hal_psram_init).
     *
     * The shadow buffer and its dirty-tile/FASTGFX machinery are indexed
     * with the landscape 320-wide stride, so they are only valid in
     * landscape. Portrait orientations (s_w/s_h swapped to 240x320, latched
     * below) keep the shadow NULL and use the same direct-to-panel path as
     * the no-PSRAM boards — MISO readback still works, console scroll falls
     * back to clear-home. */
    if (!s_shadow && PSRAMsize && DISPLAY_LANDSCAPE) {
        s_shadow = heap_caps_malloc((size_t)LCD_W * LCD_H * sizeof(*s_shadow),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_shadow)
            ESP_LOGW(TAG, "shadow allocation failed; direct-to-panel mode");
    }
    if (!s_flush) {
        s_flush = heap_caps_malloc(LCD_FLUSH_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_flush)
            ESP_LOGW(TAG, "DMA flush buffer allocation failed; using row flush");
        else
            ESP_LOGI(TAG, "DMA flush buffer ready: %u bytes / %d rows",
                     (unsigned)LCD_FLUSH_BYTES, LCD_FLUSH_ROWS);
    }

    /* Latch the configured controller + orientation. The web console
     * rewrites Option.DISPLAY_TYPE live, so keep a private copy. Portrait
     * orientations swap the 320x240 landscape binding to 240x320. */
    s_panel_type = (Option.DISPLAY_TYPE == ST7789B) ? ST7789B : ILI9341;
    if (DISPLAY_LANDSCAPE) {
        s_w = LCD_W;
        s_h = LCD_H;
    } else {
        s_w = LCD_H;
        s_h = LCD_W;
    }

    lcd_init_controller();

    if (!s_shadow) panel_miso_diag();

    esp32_ili9341_lcd_bind_panel();
    Option.DefaultFont = 0x01;
    Option.ColourCode = 1;
    if (Option.DefaultFC == 0 && Option.DefaultBC == 0) {
        Option.DefaultFC = WHITE;
        Option.DefaultBC = BLACK;
    }
    esp32_board_profile_reserve_lcd_pins();
    ApplyDefaultConsoleColours();
    CurrentX = 0;
    CurrentY = 0;
    ClearScreen(gui_bcolour);
    ESP_LOGI(TAG, "ILI9341 ready: sclk=%d mosi=%d miso=%d cs=%d dc=%d",
             sclk, mosi, miso, cs, dc);
}
