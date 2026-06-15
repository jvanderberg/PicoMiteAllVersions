/*
 * esp32_cyd_esp_lcd.c - CYD SPI-LCD backend built on ESP-IDF's esp_lcd.
 *
 * Replaces the hand-rolled SPI/CS/readback path with Espressif's esp_lcd
 * panel framework: esp_lcd_panel_io_spi for the transport and the built-in
 * ST7789 panel driver. The classic-ESP32 CYD has no PSRAM, so this
 * presents straight to the panel with no permanent framebuffer; MMBasic's
 * Draw.c function pointers are bridged to esp_lcd_panel_draw_bitmap.
 *
 * Exposes the same symbol surface app_main / console / wifi / framebuffer /
 * fastgfx expect from the old esp32_ili9341_lcd.c, so those callers are
 * unchanged.
 */

#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "SPI-LCD.h"
#include "esp32_backlight.h"
#include "esp32_board_profile.h"

#define LCD_W 320
#define LCD_H 240
#define LCD_SPI_HOST SPI3_HOST
/* Writes run fast; panel RAM reads use the separate slow read device below. */
#define LCD_SPI_HZ 40000000
#define LCD_SPI_RD_HZ 8000000
#define LCD_RD_INPUT_DELAY_NS 80
#define LCD_CASET 0x2A
#define LCD_PASET 0x2B
#define LCD_RAMWR 0x2C
#define LCD_RAMRD 0x2E
#define SCRATCH_PX (LCD_W * 16) /* tile writes through this many pixels */
#define READ_SCRATCH_PX (LCD_W * 4)

static const char * TAG = "esp_lcd";
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static spi_device_handle_t s_lcd; /* fast write device used after esp_lcd init */
static spi_device_handle_t s_rd;  /* slow RAMRD device */
static int s_dc = -1;            /* DC GPIO, toggled manually around RAMRD */
static int s_cs = -1;
static int s_w = LCD_W;
static int s_h = LCD_H;
static int s_panel_type = ST7789B;
static uint8_t * s_scratch;   /* DMA write scratch, SCRATCH_PX RGB565 pixels */
static uint8_t * s_rdbuf;     /* DMA read scratch, 1 dummy + READ_SCRATCH_PX*3 */

extern volatile int DISPLAY_TYPE;
extern unsigned char OptionConsole;
extern const int colours[16];
extern int RGB121map[16];
extern void port_set_error_restore_console_surface_callback(void (*callback)(void));

void esp32_ili9341_lcd_scroll(int lines);

static inline uint16_t rgb565(int c) {
    uint32_t v = (uint32_t)c;
    return (uint16_t)(((v & 0x00f80000u) >> 8) | ((v & 0x0000fc00u) >> 5) |
                      ((v & 0x000000f8u) >> 3));
}

static inline void put565(uint8_t * p, int c) {
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

static int clip_rect(int * x1, int * y1, int * x2, int * y2) {
    if (*x1 > *x2) { int t = *x1; *x1 = *x2; *x2 = t; }
    if (*y1 > *y2) { int t = *y1; *y1 = *y2; *y2 = t; }
    if (*x1 < 0) *x1 = 0;
    if (*y1 < 0) *y1 = 0;
    if (*x2 >= s_w) *x2 = s_w - 1;
    if (*y2 >= s_h) *y2 = s_h - 1;
    return *x1 <= *x2 && *y1 <= *y2;
}

static void raw_tx_locked(const void * data, size_t len, int dc) {
    if (!s_lcd || !data || len == 0) return;
    gpio_set_level((gpio_num_t)s_dc, dc);
    spi_transaction_t t = {0};
    t.length = len * 8;
    t.tx_buffer = data;
    esp_err_t err = spi_device_polling_transmit(s_lcd, &t);
    if (err != ESP_OK) ESP_LOGW(TAG, "spi transmit failed: %s", esp_err_to_name(err));
}

static void raw_addr_window(int x1, int y1, int x2, int y2) {
    uint8_t b[4];
    b[0] = (uint8_t)(x1 >> 8);
    b[1] = (uint8_t)x1;
    b[2] = (uint8_t)(x2 >> 8);
    b[3] = (uint8_t)x2;
    raw_tx_locked(&(uint8_t){LCD_CASET}, 1, 0);
    raw_tx_locked(b, sizeof(b), 1);
    b[0] = (uint8_t)(y1 >> 8);
    b[1] = (uint8_t)y1;
    b[2] = (uint8_t)(y2 >> 8);
    b[3] = (uint8_t)y2;
    raw_tx_locked(&(uint8_t){LCD_PASET}, 1, 0);
    raw_tx_locked(b, sizeof(b), 1);
    raw_tx_locked(&(uint8_t){LCD_RAMWR}, 1, 0);
}

static void raw_read_window(int x1, int y1, int x2, int y2) {
    raw_addr_window(x1, y1, x2, y2);
    raw_tx_locked(&(uint8_t){LCD_RAMRD}, 1, 0);
}

/* Push an already-RGB565-filled region to the panel. */
static void panel_blit(int x1, int y1, int x2, int y2, const uint8_t * px) {
    if (!s_lcd) {
        if (s_panel) esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2 + 1, y2 + 1, px);
        return;
    }
    gpio_set_level((gpio_num_t)s_cs, 0);
    raw_addr_window(x1, y1, x2, y2);
    raw_tx_locked(px, (size_t)(x2 - x1 + 1) * (size_t)(y2 - y1 + 1) * 2u, 1);
    gpio_set_level((gpio_num_t)s_cs, 1);
}

static void esp_lcd_draw_rectangle(int x1, int y1, int x2, int y2, int c) {
    if (!clip_rect(&x1, &y1, &x2, &y2) || !s_scratch) return;
    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    uint8_t col[2];
    put565(col, c);
    int rows = SCRATCH_PX / w;
    if (rows < 1) rows = 1;
    if (rows > h) rows = h;
    for (int i = 0; i < w * rows; i++) {
        s_scratch[i * 2] = col[0];
        s_scratch[i * 2 + 1] = col[1];
    }
    for (int y = 0; y < h;) {
        int n = h - y;
        if (n > rows) n = rows;
        panel_blit(x1, y1 + y, x2, y1 + y + n - 1, s_scratch);
        y += n;
    }
}

static void esp_lcd_draw_pixel(int x, int y, int c) {
    if (x < 0 || y < 0 || x >= s_w || y >= s_h || !s_scratch) return;
    put565(s_scratch, c);
    panel_blit(x, y, x, y, s_scratch);
}

static void esp_lcd_draw_bitmap(int x, int y, int width, int height, int scale,
                                int fc, int bc, unsigned char * bitmap) {
    if (!bitmap || width <= 0 || height <= 0 || !s_scratch) return;
    if (scale < 1) scale = 1;
    int total_bits = width * height;
    int out_w = width * scale;
    int out_h = height * scale;
    int cx1 = x, cy1 = y, cx2 = x + out_w - 1, cy2 = y + out_h - 1;
    if (!clip_rect(&cx1, &cy1, &cx2, &cy2)) return;
    int w = cx2 - cx1 + 1;
    uint8_t fc565[2];
    put565(fc565, fc);
    /* Transparent background can't be expressed in a single bitmap push, so
     * only the foreground runs are written per row. Opaque background fills
     * the whole clipped row. */
    if (bc < 0) {
        for (int py = cy1; py <= cy2; py++) {
            int row = (py - y) / scale;
            int run_x = -1, run_w = 0;
            for (int px = cx1; px <= cx2; px++) {
                int col = (px - x) / scale;
                int bit = row * width + col;
                int on = (bitmap[bit / 8] >> ((total_bits - bit - 1) % 8)) & 1;
                if (!on) {
                    if (run_w) { panel_blit(run_x, py, run_x + run_w - 1, py, s_scratch); run_w = 0; run_x = -1; }
                    continue;
                }
                if (run_x < 0) run_x = px;
                s_scratch[run_w * 2] = fc565[0];
                s_scratch[run_w * 2 + 1] = fc565[1];
                run_w++;
            }
            if (run_w) panel_blit(run_x, py, run_x + run_w - 1, py, s_scratch);
        }
        return;
    }
    uint8_t fcbytes[2], bcbytes[2];
    put565(fcbytes, fc);
    put565(bcbytes, bc);
    int rows = SCRATCH_PX / w;
    if (rows < 1) rows = 1;
    for (int py = cy1; py <= cy2;) {
        int nr = cy2 - py + 1;
        if (nr > rows) nr = rows;
        for (int ry = 0; ry < nr; ry++) {
            int row = (py + ry - y) / scale;
            uint8_t * out = s_scratch + (size_t)ry * (size_t)w * 2u;
            for (int px = cx1; px <= cx2; px++) {
                int col = (px - x) / scale;
                int bit = row * width + col;
                int on = (bitmap[bit / 8] >> ((total_bits - bit - 1) % 8)) & 1;
                const uint8_t * src = on ? fcbytes : bcbytes;
                size_t o = (size_t)(px - cx1) * 2u;
                out[o] = src[0];
                out[o + 1] = src[1];
            }
        }
        panel_blit(cx1, py, cx2, py + nr - 1, s_scratch);
        py += nr;
    }
}

static void esp_lcd_draw_buffer(int x1, int y1, int x2, int y2, unsigned char * bgr) {
    if (!bgr || !s_scratch) return;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    int src_w = x2 - x1 + 1;
    int cx1 = x1, cy1 = y1, cx2 = x2, cy2 = y2;
    if (!clip_rect(&cx1, &cy1, &cx2, &cy2)) return;
    int w = cx2 - cx1 + 1;
    int rows = SCRATCH_PX / w;
    if (rows < 1) rows = 1;
    for (int py = cy1; py <= cy2;) {
        int nr = cy2 - py + 1;
        if (nr > rows) nr = rows;
        for (int ry = 0; ry < nr; ry++) {
            unsigned char * src = bgr + ((size_t)(py + ry - y1) * (size_t)src_w +
                                         (size_t)(cx1 - x1)) * 3u;
            uint8_t * out = s_scratch + (size_t)ry * (size_t)w * 2u;
            for (int x = 0; x < w; x++) {
                int c = ((int)src[2] << 16) | ((int)src[1] << 8) | src[0];
                put565(out + (size_t)x * 2u, c);
                src += 3;
            }
        }
        panel_blit(cx1, py, cx2, py + nr - 1, s_scratch);
        py += nr;
    }
}

static void esp_lcd_draw_buffer_fast(int x1, int y1, int x2, int y2, int blank,
                                     unsigned char * p) {
    if (!p || !s_scratch) return;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    int src_w = x2 - x1 + 1;
    int cx1 = x1, cy1 = y1, cx2 = x2, cy2 = y2;
    if (!clip_rect(&cx1, &cy1, &cx2, &cy2)) return;
    for (int y = cy1; y <= cy2; y++) {
        int run_x = -1, run_w = 0;
        for (int x = cx1; x <= cx2; x++) {
            int src_index = (y - y1) * src_w + (x - x1);
            uint8_t nibble = packed_rgb121_get(p, src_index);
            if (blank != -1 && nibble == sprite_transparent) {
                if (run_w) { panel_blit(run_x, y, run_x + run_w - 1, y, s_scratch); run_w = 0; run_x = -1; }
                continue;
            }
            if (run_x < 0) run_x = x;
            put565(s_scratch + (size_t)run_w * 2u, rgb121_palette_colour(nibble));
            run_w++;
        }
        if (run_w) panel_blit(run_x, y, run_x + run_w - 1, y, s_scratch);
    }
}

/* Read a region of panel RAM. CS is held low across CASET/PASET/RAMRD and the
 * read clocks; there is no bus acquire and no CS_KEEP transaction flag. */
static int panel_read_region_stride(int x1, int y1, int x2, int y2,
                                    uint8_t * out_bgr, int out_stride_px) {
    if (!s_lcd || !s_rd || !s_rdbuf) return 0;
    int w = x2 - x1 + 1, h = y2 - y1 + 1;
    if (w <= 0 || h <= 0 || out_stride_px < w) return 0;
    int per_row = w * 3;
    int rows = (1 + READ_SCRATCH_PX * 3) / per_row;
    if (rows < 1) rows = 1;
    if (rows > h) rows = h;
    for (int y = 0; y < h;) {
        int nr = h - y;
        if (nr > rows) nr = rows;
        int ys = y1 + y, ye = y1 + y + nr - 1;
        int nbytes = 1 + nr * per_row; /* 1 leading dummy byte */
        gpio_set_level((gpio_num_t)s_cs, 0);
        raw_read_window(x1, ys, x2, ye);
        gpio_set_level((gpio_num_t)s_dc, 1);
        spi_transaction_t t = {0};
        t.length = (size_t)nbytes * 8;
        t.rxlength = (size_t)nbytes * 8;
        t.rx_buffer = s_rdbuf;
        esp_err_t err = spi_device_polling_transmit(s_rd, &t);
        gpio_set_level((gpio_num_t)s_dc, 0);
        gpio_set_level((gpio_num_t)s_cs, 1);
        if (err != ESP_OK) return 0;
        uint8_t * src = s_rdbuf + 1; /* skip the RAMRD dummy byte */
        for (int ry = 0; ry < nr; ry++) {
            for (int x = 0; x < w; x++) {
                size_t si = ((size_t)ry * (size_t)w + (size_t)x) * 3u;
                size_t o = ((size_t)(y + ry) * (size_t)out_stride_px +
                            (size_t)x) * 3u;
                out_bgr[o + 0] = src[si + 2];
                out_bgr[o + 1] = src[si + 1];
                out_bgr[o + 2] = src[si + 0];
            }
        }
        y += nr;
    }
    return 1;
}

static int panel_read_region(int x1, int y1, int x2, int y2, uint8_t * out_bgr) {
    return panel_read_region_stride(x1, y1, x2, y2, out_bgr, x2 - x1 + 1);
}

static void esp_lcd_read_buffer(int x1, int y1, int x2, int y2, unsigned char * bgr) {
    if (!bgr) return;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    int out_w = x2 - x1 + 1;
    int out_h = y2 - y1 + 1;
    memset(bgr, 0, (size_t)out_w * (size_t)out_h * 3u);
    int cx1 = x1, cy1 = y1, cx2 = x2, cy2 = y2;
    if (!clip_rect(&cx1, &cy1, &cx2, &cy2)) return;
    uint8_t * out = bgr + ((size_t)(cy1 - y1) * (size_t)out_w +
                           (size_t)(cx1 - x1)) * 3u;
    panel_read_region_stride(cx1, cy1, cx2, cy2, out, out_w);
}

static void esp_lcd_read_buffer_fast(int x1, int y1, int x2, int y2, unsigned char * p) {
    if (!p) return;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    memset(p, 0, ((size_t)w * (size_t)h + 1u) / 2u);
    static uint8_t row[LCD_W * 3]; /* one clipped B,G,R row, off-stack */
    for (int y = y1; y <= y2; y++) {
        if (y >= 0 && y < s_h) {
            int rx1 = x1 < 0 ? 0 : x1;
            int rx2 = x2 >= s_w ? s_w - 1 : x2;
            if (rx1 <= rx2 && panel_read_region(rx1, y, rx2, y, row)) {
                int base = (y - y1) * w + (rx1 - x1);
                for (int x = 0; x <= rx2 - rx1; x++) {
                    size_t o = (size_t)x * 3u;
                    int c = (row[o + 2] << 16) | (row[o + 1] << 8) | row[o + 0];
                    packed_rgb121_set(p, base + x, RGB121(c));
                }
            }
        }
    }
}

int esp32_ili9341_lcd_ready(void) {
    return (s_panel != NULL || s_lcd != NULL) && Option.DISPLAY_TYPE == s_panel_type && HRes == s_w &&
           VRes == s_h;
}

int esp32_ili9341_lcd_fastgfx_ready(void) {
    return 0;
}

/* The no-PSRAM CYD presents straight to the panel; there is no off-screen
 * buffer to back an MMBasic FRAMEBUFFER, so the command is rejected. */
int esp32_ili9341_lcd_framebuffer_supported(void) {
    return 0;
}

static void bind_panel(void) {
    HRes = DisplayHRes = s_w;
    VRes = DisplayVRes = s_h;
    ScreenSize = s_w * s_h;
    Option.DISPLAY_TYPE = s_panel_type;
    DISPLAY_TYPE = s_panel_type;
    Option.DISPLAY_CONSOLE = 1;
    Option.Refresh = 0;
    OptionConsole = 3;
    DrawPixel = esp_lcd_draw_pixel;
    DrawRectangle = esp_lcd_draw_rectangle;
    DrawBitmap = esp_lcd_draw_bitmap;
    DrawBuffer = esp_lcd_draw_buffer;
    DrawBLITBuffer = esp_lcd_draw_buffer;
    DrawBufferFast = esp_lcd_draw_buffer_fast;
    if (s_rd) {
        /* Readback is available for PIXEL()/sprites and for console scroll.
         * Use the CYD-local scroll implementation; the shared SPI-LCD scroll
         * path assumes the Pico SPI backend state and can blank the console. */
        ReadBuffer = esp_lcd_read_buffer;
        ReadBLITBuffer = esp_lcd_read_buffer;
        ReadBufferFast = esp_lcd_read_buffer_fast;
        ScrollLCD = esp32_ili9341_lcd_scroll;
        Option.NoScroll = 0;
    } else {
        /* No MISO / read device: PIXEL()/sprites/transparent error cleanly and
         * the console clear-homes on overflow instead of scrolling. */
        ReadBuffer = (void (*)(int, int, int, int, unsigned char *))DisplayNotSet;
        ReadBLITBuffer = (void (*)(int, int, int, int, unsigned char *))DisplayNotSet;
        ReadBufferFast = (void (*)(int, int, int, int, unsigned char *))DisplayNotSet;
        ScrollLCD = (void (*)(int))DisplayNotSet;
        Option.NoScroll = 1;
    }
}

int esp32_ili9341_lcd_restore_panel(void) {
    if (!s_lcd && !s_panel) return 0;
    bind_panel();
    esp32_board_profile_reserve_lcd_pins();
    return 1;
}

void esp32_ili9341_lcd_flush_pending(void) {}

static void esp_lcd_error_restore_console_surface(void) {
    if (!esp32_ili9341_lcd_restore_panel()) return;
    Option.DISPLAY_CONSOLE = 1;
    OptionConsole = 3;
    ApplyDefaultConsoleColours();
    CurrentX = 0;
    CurrentY = 0;
}

/* Console scroll with no shadow buffer: read each shifted row straight off the
 * panel and redraw it, then fill the vacated band with the background. Only
 * bound when the read device exists (otherwise the console clear-homes). */
void esp32_ili9341_lcd_scroll(int lines) {
    if (!s_rd || lines == 0) return;
    ScrollStart = 0;
    int n = lines < 0 ? -lines : lines;
    if (n >= s_h) { esp_lcd_draw_rectangle(0, 0, s_w - 1, s_h - 1, gui_bcolour); return; }
    static uint8_t row[LCD_W * 3]; /* one BGR row, off-stack */
    if (lines > 0) {
        for (int y = 0; y < s_h - lines; y++) {
            panel_read_region(0, y + lines, s_w - 1, y + lines, row);
            esp_lcd_draw_buffer(0, y, s_w - 1, y, row);
        }
        esp_lcd_draw_rectangle(0, s_h - lines, s_w - 1, s_h - 1, gui_bcolour);
    } else {
        for (int y = s_h - 1; y >= n; y--) {
            panel_read_region(0, y - n, s_w - 1, y - n, row);
            esp_lcd_draw_buffer(0, y, s_w - 1, y, row);
        }
        esp_lcd_draw_rectangle(0, 0, s_w - 1, n - 1, gui_bcolour);
    }
}

void esp32_ili9341_lcd_snapshot_rgb121(uint8_t * out) {
    (void)out;
}

static uint8_t * lcd_present_batch_buffer(int width, int * rows_per_batch) {
    int rows = SCRATCH_PX / width;
    if (rows < 1) rows = 1;
    if (rows_per_batch) *rows_per_batch = rows;
    return s_scratch;
}

void esp32_ili9341_lcd_present_rgb121_diff(uint8_t * back, uint8_t * front) {
    (void)back;
    (void)front;
}

void esp32_ili9341_lcd_present_rgb121_rect(const uint8_t * src, int xs, int xe, int ys, int ye, int odd) {
    if (!esp32_ili9341_lcd_ready() || !s_scratch || !src) return;
    int src_w = xe - xs + 1;
    if (src_w <= 0) return;
    int cx1 = xs, cy1 = ys, cx2 = xe, cy2 = ye;
    if (!clip_rect(&cx1, &cy1, &cx2, &cy2)) return;
    int w = cx2 - cx1 + 1;
    int h = cy2 - cy1 + 1;
    int rows_per_batch;
    uint8_t * tx = lcd_present_batch_buffer(w, &rows_per_batch);

    for (int yoff = 0; yoff < h;) {
        int rows = h - yoff;
        if (rows > rows_per_batch) rows = rows_per_batch;
        for (int ry = 0; ry < rows; ry++) {
            int y = cy1 + yoff + ry;
            int src_row_base = (y - ys) * src_w;
            uint8_t * out = tx + (size_t)ry * (size_t)w * 2u;
            for (int x = cx1; x <= cx2; x++) {
                int src_index = src_row_base + (x - xs);
                int packed_index = src_index + (odd ? 1 : 0);
                uint8_t byte = src[(size_t)packed_index >> 1];
                uint8_t nibble = (packed_index & 1) ? (uint8_t)(byte >> 4)
                                                    : (uint8_t)(byte & 0x0f);
                put565(&out[(size_t)(x - cx1) * 2u], rgb121_palette_colour(nibble));
            }
        }
        panel_blit(cx1, cy1 + yoff, cx2, cy1 + yoff + rows - 1, tx);
        yoff += rows;
    }
}

static int lcd_option_gpio(int pin) {
    if (pin <= 0 || pin > NBRPINS || (PinDef[pin].mode & UNUSED)) return -1;
    return PinDef[pin].GPno;
}

static void lcd_release_resources(int bus_inited) {
    if (s_rd) {
        (void)spi_bus_remove_device(s_rd);
        s_rd = NULL;
    }
    if (s_lcd) {
        (void)spi_bus_remove_device(s_lcd);
        s_lcd = NULL;
    }
    if (s_panel) {
        (void)esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
    if (s_io) {
        (void)esp_lcd_panel_io_del(s_io);
        s_io = NULL;
    }
    if (bus_inited) (void)spi_bus_free(LCD_SPI_HOST);
    if (s_scratch) {
        heap_caps_free(s_scratch);
        s_scratch = NULL;
    }
    if (s_rdbuf) {
        heap_caps_free(s_rdbuf);
        s_rdbuf = NULL;
    }
    s_dc = -1;
    s_cs = -1;
}

void esp32_ili9341_lcd_init(void) {
    if (Option.WebConsole || s_lcd || s_panel) return;
    if (!Option.LCD_CD || !Option.LCD_CS) return;
    const int sclk = lcd_option_gpio(Option.LCD_CLK);
    const int mosi = lcd_option_gpio(Option.LCD_MOSI);
    const int miso = lcd_option_gpio(Option.LCD_MISO);
    const int cs = lcd_option_gpio(Option.LCD_CS);
    const int dc = lcd_option_gpio(Option.LCD_CD);
    const int rst = lcd_option_gpio(Option.LCD_Reset);
    const int backlight = lcd_option_gpio(Option.DISPLAY_BL);
    if (sclk < 0 || mosi < 0 || cs < 0 || dc < 0) {
        ESP_LOGW(TAG, "LCD pins incomplete");
        return;
    }

    s_panel_type = (Option.DISPLAY_TYPE == ST7789B) ? ST7789B : ILI9341;
    if (Option.DISPLAY_ORIENTATION & 1) { s_w = LCD_W; s_h = LCD_H; }
    else { s_w = LCD_H; s_h = LCD_W; }

    spi_bus_config_t bus = {
        .mosi_io_num = mosi,
        .miso_io_num = miso,
        .sclk_io_num = sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SCRATCH_PX * 3 + 16,
    };
    esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = cs,
        .dc_gpio_num = dc,
        .spi_mode = 0,
        .pclk_hz = LCD_SPI_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg,
                                   &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel_io_spi failed: %s", esp_err_to_name(err));
        lcd_release_resources(1);
        return;
    }

    s_dc = dc;

    /* Keep the same MADCTL colour-order bit as the known-working CYD init.
     * The visible red/blue swap is corrected in the pixel packer, not by
     * changing panel init state. */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = rst, /* -1 if none (software reset is used) */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new_panel_st7789 failed: %s", esp_err_to_name(err));
        lcd_release_resources(1);
        return;
    }

    err = esp_lcd_panel_reset(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel reset failed: %s", esp_err_to_name(err));
        lcd_release_resources(1);
        return;
    }
    err = esp_lcd_panel_init(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
        lcd_release_resources(1);
        return;
    }
    /* CYD2USB ST7789 wants inversion off; OPTION INVERT (Option.BGR=1) means
     * "no inversion" on this controller, matching the old ST7789 path. */
    err = esp_lcd_panel_invert_color(s_panel, !Option.BGR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel invert failed: %s", esp_err_to_name(err));
        lcd_release_resources(1);
        return;
    }
    /* Landscape = swap X/Y; flip the mirror for the 180° variants. */
    int landscape = Option.DISPLAY_ORIENTATION & 1;
    int flip = (Option.DISPLAY_ORIENTATION == RLANDSCAPE ||
                Option.DISPLAY_ORIENTATION == RPORTRAIT);
    err = esp_lcd_panel_swap_xy(s_panel, landscape);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel swap_xy failed: %s", esp_err_to_name(err));
        lcd_release_resources(1);
        return;
    }
    err = esp_lcd_panel_mirror(s_panel, landscape ^ flip, flip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel mirror failed: %s", esp_err_to_name(err));
        lcd_release_resources(1);
        return;
    }
    err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel display-on failed: %s", esp_err_to_name(err));
        lcd_release_resources(1);
        return;
    }

    esp_lcd_panel_del(s_panel);
    s_panel = NULL;
    esp_lcd_panel_io_del(s_io);
    s_io = NULL;

    s_dc = dc;
    s_cs = cs;
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << (uint32_t)s_dc) | (1ULL << (uint32_t)s_cs),
        .mode = GPIO_MODE_OUTPUT,
    };
    err = gpio_config(&out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        lcd_release_resources(1);
        return;
    }
    gpio_set_level((gpio_num_t)s_cs, 1);

    spi_device_interface_config_t wr_cfg = {
        .clock_speed_hz = LCD_SPI_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
        .flags = SPI_DEVICE_NO_DUMMY,
    };
    err = spi_bus_add_device(LCD_SPI_HOST, &wr_cfg, &s_lcd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD write device add failed: %s", esp_err_to_name(err));
        lcd_release_resources(1);
        return;
    }

    spi_device_interface_config_t rd_cfg = {
        .clock_speed_hz = LCD_SPI_RD_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
        .input_delay_ns = LCD_RD_INPUT_DELAY_NS,
    };
    if (miso >= 0) {
        err = spi_bus_add_device(LCD_SPI_HOST, &rd_cfg, &s_rd);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "RAMRD read device add failed; reads disabled: %s",
                     esp_err_to_name(err));
            s_rd = NULL;
        }
    }

    if (!s_scratch)
        s_scratch = heap_caps_malloc(SCRATCH_PX * 2u,
                                     MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_scratch) {
        ESP_LOGE(TAG, "LCD scratch allocation failed");
        lcd_release_resources(1);
        return;
    }
    if (s_rd && !s_rdbuf)
        s_rdbuf = heap_caps_malloc(1 + READ_SCRATCH_PX * 3,
                                   MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (s_rd && !s_rdbuf) {
        ESP_LOGW(TAG, "RAMRD scratch allocation failed; reads disabled");
        (void)spi_bus_remove_device(s_rd);
        s_rd = NULL;
    }

    if (backlight >= 0) esp32_backlight_init_default();

    bind_panel();
    Option.DefaultFont = 0x01;
    Option.ColourCode = 1;
    if (Option.DefaultFC == 0 && Option.DefaultBC == 0) {
        Option.DefaultFC = WHITE;
        Option.DefaultBC = BLACK;
    }
    esp32_board_profile_reserve_lcd_pins();
    port_set_error_restore_console_surface_callback(esp_lcd_error_restore_console_surface);
    ApplyDefaultConsoleColours();
    CurrentX = 0;
    CurrentY = 0;
    ClearScreen(gui_bcolour);
    ESP_LOGI(TAG, "esp_lcd ST7789 ready: %dx%d sclk=%d mosi=%d miso=%d cs=%d dc=%d",
             s_w, s_h, sclk, mosi, miso, cs, dc);
}

/* The CYD panel is presented to glass on every draw, so the manual REFRESH
 * command is a no-op and OPTION DISPLAY AUTOREFRESH ON is rejected. */
int port_display_manual_refresh_supported(void) { return 0; }
