/*
 * drivers/vga_lcdcam_s3/vga_lcdcam_s3.c — ESP32-S3 VGA scanout via LCD_CAM.
 *
 * Drives an external resistor-ladder DAC (VGA666-style, wired for RGB332)
 * with a standard 640x480@60 VGA signal using the ESP32-S3 LCD_CAM RGB
 * panel peripheral. The peripheral allocates a single RGB332 frame buffer
 * in PSRAM and feeds the LCD DMA through internal-RAM bounce buffers.
 */

#include "vga_lcdcam_s3.h"

#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_private/startup_internal.h"

static const char * TAG = "vga_lcdcam";

static esp_lcd_panel_handle_t s_panel = NULL;
static uint8_t * s_fb = NULL;
static bool s_cpu_bounce_scanout = false;

/* Live scanout source for the bounce-fill ISR. The panel runs in no_fb
 * mode: every 16-line bounce buffer is filled straight from the current
 * source, so a 320-wide mode never materialises a 640x480 PSRAM frame —
 * the per-chunk PSRAM read drops from 10240 to 2560 bytes and the
 * expansion write lands in internal RAM. Writes to the source buffer
 * are visible on screen within a frame with no present step. */
typedef enum {
    VGA_SRC_640 = 0, /* s_fb, plain copy (console mode) */
    VGA_SRC_320_2X,  /* 320x240 source, 2x pixel/line doubling */
    VGA_SRC_320_2X_DITHER,
} vga_src_mode_t;

static const uint8_t * volatile s_src = NULL;
static volatile int s_src_mode = VGA_SRC_640;

/* Signalled once per frame at the start-of-blanking edge; see
 * vga_lcdcam_s3_wait_vsync. */
static SemaphoreHandle_t s_frame_edge_sem = NULL;

static bool IRAM_ATTR vga_frame_edge_cb(esp_lcd_panel_handle_t panel,
                                        const esp_lcd_rgb_panel_event_data_t * edata,
                                        void * user_ctx) {
    (void)panel;
    (void)edata;
    (void)user_ctx;
    BaseType_t woken = pdFALSE;
    if (s_frame_edge_sem) xSemaphoreGiveFromISR(s_frame_edge_sem, &woken);
    return woken == pdTRUE;
}

static void vga_lcdcam_s3_set_scanout_640(void) {
    s_src = s_fb; /* live-source marker: presents into the fb still run */
    s_src_mode = VGA_SRC_640;
}

/*
 * Standard VESA 640x480@60 timing. Pixel clock is nominally 25.175 MHz;
 * the LCD_CAM clock divider lands on the nearest achievable frequency,
 * which every VGA monitor tested tolerates. Both sync signals are
 * negative polarity (idle high, pulse low), so hsync/vsync_idle_low stay
 * 0. de is unused — a resistor DAC has no data-enable input.
 */
static uint32_t vga_pclk_hz(uint8_t clock_mode) {
    switch (clock_mode) {
    case VGA_LCDCAM_CLOCK_25MHZ:
    case VGA_LCDCAM_CLOCK_25MHZ240:
        return 25000000;
    case VGA_LCDCAM_CLOCK_STANDARD:
    case VGA_LCDCAM_CLOCK_PLL240:
    default:
        return 25175000;
    }
}

static lcd_clock_source_t vga_clock_source(uint8_t clock_mode) {
    switch (clock_mode) {
    case VGA_LCDCAM_CLOCK_PLL240:
    case VGA_LCDCAM_CLOCK_25MHZ240:
        return LCD_CLK_SRC_PLL240M;
    case VGA_LCDCAM_CLOCK_STANDARD:
    case VGA_LCDCAM_CLOCK_25MHZ:
    default:
        return LCD_CLK_SRC_DEFAULT;
    }
}

static const char * vga_clock_name(uint8_t clock_mode) {
    switch (clock_mode) {
    case VGA_LCDCAM_CLOCK_PLL240:
        return "PLL240";
    case VGA_LCDCAM_CLOCK_25MHZ:
        return "25MHZ";
    case VGA_LCDCAM_CLOCK_25MHZ240:
        return "25MHZ240";
    case VGA_LCDCAM_CLOCK_STANDARD:
    default:
        return "STANDARD";
    }
}

static void vga_apply_drive(const vga_lcdcam_pins_t * pins) {
    gpio_drive_cap_t drive = (pins->drive_cap <= GPIO_DRIVE_CAP_3)
                                 ? (gpio_drive_cap_t)pins->drive_cap
                                 : GPIO_DRIVE_CAP_DEFAULT;
    for (int i = 0; i < 8; i++) {
        if (pins->data_gpio[i] >= 0) gpio_set_drive_capability((gpio_num_t)pins->data_gpio[i], drive);
    }
    if (pins->hsync_gpio >= 0) gpio_set_drive_capability((gpio_num_t)pins->hsync_gpio, drive);
    if (pins->vsync_gpio >= 0) gpio_set_drive_capability((gpio_num_t)pins->vsync_gpio, drive);
    if (pins->pclk_gpio >= 0) gpio_set_drive_capability((gpio_num_t)pins->pclk_gpio, drive);
}

static esp_lcd_rgb_timing_t vga_640x480_60(uint8_t sync_flags, uint8_t clock_mode) {
    esp_lcd_rgb_timing_t t = {
        .pclk_hz = vga_pclk_hz(clock_mode),
        .h_res = VGA_LCDCAM_HRES,
        .v_res = VGA_LCDCAM_VRES,
        .hsync_pulse_width = 96,
        .hsync_back_porch = 48,
        .hsync_front_porch = 16,
        .vsync_pulse_width = 2,
        .vsync_back_porch = 33,
        .vsync_front_porch = 10,
        .flags = {
            .hsync_idle_low = (sync_flags & VGA_LCDCAM_SYNC_HSYNC_IDLE_LOW) != 0,
            .vsync_idle_low = (sync_flags & VGA_LCDCAM_SYNC_VSYNC_IDLE_LOW) != 0,
            .de_idle_high = 0,
            .pclk_active_neg = 0,
            .pclk_idle_high = 0,
        },
    };
    return t;
}

/* Forked esp_lcd RGB panel (vga_lcd_rgb_320d.c): line-doubled 320-wide
 * scanout from an internal-SRAM frame buffer, no CPU in the path. */
extern esp_err_t vga320d_new_rgb_panel(const esp_lcd_rgb_panel_config_t * config,
                                       esp_lcd_panel_handle_t * ret_panel);
extern esp_err_t vga320d_rgb_panel_register_event_callbacks(
    esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_callbacks_t * callbacks,
    void * user_ctx);
extern esp_err_t vga320d_rgb_panel_get_frame_buffer(esp_lcd_panel_handle_t panel,
                                                    uint32_t fb_num, void ** fb0, ...);
extern uint8_t * vga320d_reserved_fb;
extern size_t vga320d_reserved_fb_size;

static vga_lcdcam_pins_t s_pins;
static int s_panel_kind;   /* 0 = 640 bounce-fill panel, 1 = 320d panel */
static uint8_t * s_fb320;  /* live 320d frame buffer while kind == 1 */
static bool s_fb320_psram; /* 320d fb is in PSRAM: writes need cache sync */

static void vga_fill_pin_config(esp_lcd_rgb_panel_config_t * cfg) {
    cfg->hsync_gpio_num = s_pins.hsync_gpio;
    cfg->vsync_gpio_num = s_pins.vsync_gpio;
    cfg->de_gpio_num = -1; /* VGA has no data-enable line */
    cfg->pclk_gpio_num = s_pins.pclk_gpio;
    cfg->disp_gpio_num = -1;
    for (int i = 0; i < 8; i++) cfg->data_gpio_nums[i] = s_pins.data_gpio[i];
    for (int i = 8; i < 16; i++) cfg->data_gpio_nums[i] = -1;
}

static bool vga_build_640_panel(void) {
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = vga_clock_source(s_pins.clock_mode),
        .timings = vga_640x480_60(s_pins.sync_flags, s_pins.clock_mode),
        .data_width = 8, /* RGB332 — one byte per pixel out an 8-bit bus */
        .bits_per_pixel = 8,
        /* Stock upstream shape: the panel owns a PSRAM frame buffer and
         * its ISR copies it to internal bounce buffers 16 lines at a
         * time — measured ~6.7k cycles per 10 KB chunk. A no_fb fill
         * callback doing the same copy from an identically-allocated
         * buffer cost 74k cycles per chunk (55% of core 0) for reasons
         * never fully explained; the scanout source must stay the
         * panel's own frame buffer. */
        .num_fbs = 1,
        .bounce_buffer_size_px = 16 * VGA_LCDCAM_HRES,
        .flags = {
            .fb_in_psram = 1,
        },
    };
    vga_fill_pin_config(&cfg);

    esp_err_t err = vga320d_new_rgb_panel(&cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "vga320d_new_rgb_panel(640) failed: %s", esp_err_to_name(err));
        s_panel = NULL;
        return false;
    }
    vga_apply_drive(&s_pins);
    {
        /* on_bounce_frame_finish gives vga_lcdcam_s3_wait_vsync its
         * once-per-frame edge. */
        esp_lcd_rgb_panel_event_callbacks_t cbs = {
            .on_bounce_frame_finish = vga_frame_edge_cb,
        };
        vga320d_rgb_panel_register_event_callbacks(s_panel, &cbs, NULL);
    }
    err = esp_lcd_panel_reset(s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_init(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "640 panel init failed: %s", esp_err_to_name(err));
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
        return false;
    }
    {
        /* The console buffer IS the panel's frame buffer. */
        void * pfb = NULL;
        vga320d_rgb_panel_get_frame_buffer(s_panel, 1, &pfb);
        if (pfb) s_fb = (uint8_t *)pfb;
    }
    vga_lcdcam_s3_set_scanout_640();
    s_cpu_bounce_scanout = true;
    s_panel_kind = 0;
    s_fb320 = NULL;
    return true;
}

/* Halved horizontal counts at half the pixel clock: the monitor sees
 * the same 31.47 kHz / 60 Hz signal as 640x480, each pixel just lasts
 * two clocks on the wire. v_res stays 480 output lines; the forked
 * panel emits each frame-buffer row twice via paired DMA descriptors. */
static esp_lcd_rgb_timing_t vga_320x480_60(uint8_t sync_flags, uint8_t clock_mode) {
    esp_lcd_rgb_timing_t t = {
        .pclk_hz = vga_pclk_hz(clock_mode) / 2,
        .h_res = VGA_LCDCAM_HRES / 2,
        .v_res = VGA_LCDCAM_VRES,
        .hsync_pulse_width = 48,
        .hsync_back_porch = 24,
        .hsync_front_porch = 8,
        .vsync_pulse_width = 2,
        .vsync_back_porch = 33,
        .vsync_front_porch = 10,
        .flags = {
            .hsync_idle_low = (sync_flags & VGA_LCDCAM_SYNC_HSYNC_IDLE_LOW) != 0,
            .vsync_idle_low = (sync_flags & VGA_LCDCAM_SYNC_VSYNC_IDLE_LOW) != 0,
            .de_idle_high = 0,
            .pclk_active_neg = 0,
            .pclk_idle_high = 0,
        },
    };
    return t;
}

static bool vga_build_320d_panel(void) {
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = vga_clock_source(s_pins.clock_mode),
        .timings = vga_320x480_60(s_pins.sync_flags, s_pins.clock_mode),
        .data_width = 8,
        .bits_per_pixel = 8,
        .num_fbs = 1, /* the fork allocs h_res x v_res/2 */
        .flags = {
            /* Internal SRAM when the boot reservation got it (zero bus
             * contention); otherwise PSRAM — at 320-wide the scanout
             * reads only 9.2 MB/s, half the 640 bounce path's load, but
             * CPU writes then need an explicit cache write-back (see
             * vga_lcdcam_s3_live_sync). */
            .fb_in_psram = vga320d_reserved_fb == NULL,
        },
    };
    vga_fill_pin_config(&cfg);

    esp_err_t err = vga320d_new_rgb_panel(&cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "vga320d_new_rgb_panel failed: %s", esp_err_to_name(err));
        s_panel = NULL;
        return false;
    }
    vga_apply_drive(&s_pins);
    {
        /* Plain fb mode: on_vsync fires for real. */
        esp_lcd_rgb_panel_event_callbacks_t cbs = {
            .on_vsync = vga_frame_edge_cb,
        };
        vga320d_rgb_panel_register_event_callbacks(s_panel, &cbs, NULL);
    }

    void * fb = NULL;
    err = vga320d_rgb_panel_get_frame_buffer(s_panel, 1, &fb);
    if (err != ESP_OK || !fb) {
        ESP_LOGE(TAG, "320d get_frame_buffer failed");
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
        return false;
    }
    memset(fb, 0, (VGA_LCDCAM_HRES / 2) * (VGA_LCDCAM_VRES / 2));
    if (fb != vga320d_reserved_fb) {
        esp_cache_msync(fb, (VGA_LCDCAM_HRES / 2) * (VGA_LCDCAM_VRES / 2),
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }

    err = esp_lcd_panel_reset(s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_init(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "320d panel init failed: %s", esp_err_to_name(err));
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
        return false;
    }
    s_fb320 = (uint8_t *)fb;
    s_fb320_psram = (fb != vga320d_reserved_fb);
    s_src = s_fb320; /* live-source marker: presents become cache syncs */
    s_cpu_bounce_scanout = false;
    s_panel_kind = 1;
    ESP_LOGI(TAG, "320x240 line-doubled scanout up; fb=%p (%s)", fb,
             s_fb320_psram ? "panel-allocated PSRAM" : "boot-reserved internal");
    return true;
}

/* Reserve the 320d scanout buffer. Callable from early boot, before any
 * other init has carved up internal DRAM: 76.8 KB contiguous internal is
 * only available while the heap is still whole, so the boot path calls
 * this right after options resolve. Idempotent; the buffer is held for
 * the life of the session and reused across every mode switch. If the
 * reservation fails, 320 modes fall back to bounce-fill expansion. */
void vga_lcdcam_s3_reserve_scanout(void) {
    if (vga320d_reserved_fb) return;
    vga320d_reserved_fb_size = (VGA_LCDCAM_HRES / 2) * (VGA_LCDCAM_VRES / 2);
    vga320d_reserved_fb = (uint8_t *)heap_caps_aligned_calloc(
        64, 1, vga320d_reserved_fb_size,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!vga320d_reserved_fb) vga320d_reserved_fb_size = 0;
}

/* Give the reservation back (boards that boot without OPTION VGA call
 * this once options resolve, so they keep their internal heap). */
void vga_lcdcam_s3_release_scanout(void) {
    if (!vga320d_reserved_fb) return;
    if (s_panel) return; /* a live panel may be using it (fb or bounce) */
    heap_caps_free(vga320d_reserved_fb);
    vga320d_reserved_fb = NULL;
    vga320d_reserved_fb_size = 0;
}

bool vga_lcdcam_s3_scanout_reserved(void) {
    return vga320d_reserved_fb != NULL;
}

/* 76.8 KB contiguous internal SRAM only exists before the FreeRTOS
 * scheduler starts: the main/idle/timer task stacks land mid-region and
 * pin the largest free block at ~49 KB forever after (a static .bss
 * buffer doesn't work either — the region is mostly converted IRAM-tail
 * that the linker cannot place data in). This runs at CORE init stage,
 * after the heap is assembled (PSRAM joins at priority 103) and before
 * any task stack is allocated. */
ESP_SYSTEM_INIT_FN(vga320d_reserve_fb, CORE, BIT(0), 200) {
    ESP_EARLY_LOGI(TAG, "320d pre-reserve: free=%u largest=%u",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA |
                                                     MALLOC_CAP_8BIT),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA |
                                                              MALLOC_CAP_8BIT));
    vga_lcdcam_s3_reserve_scanout();
    ESP_EARLY_LOGI(TAG, "320d scanout reservation: %s",
                   vga320d_reserved_fb ? "internal SRAM" : "FAILED (present fallback)");
    return ESP_OK;
}

static bool vga_lcdcam_s3_init_impl(const vga_lcdcam_pins_t * pins, uint8_t ** fb_out) {
    if (s_panel) {
        if (fb_out) *fb_out = s_fb;
        return s_fb != NULL;
    }
    if (!pins) return false;
    s_pins = *pins;

    vga_lcdcam_s3_reserve_scanout();

    if (!s_frame_edge_sem) s_frame_edge_sem = xSemaphoreCreateBinary();
    if (!vga_build_640_panel()) return false;

    if (fb_out) *fb_out = s_fb;
    ESP_LOGI(TAG, "VGA %dx%d RGB332 up; fb=%p bounce_lines=%d clock=%s req_pclk=%" PRIu32 " drive=%u",
             VGA_LCDCAM_HRES, VGA_LCDCAM_VRES, s_fb,
             s_cpu_bounce_scanout ? 16 : 0,
             vga_clock_name(s_pins.clock_mode), vga_pclk_hz(s_pins.clock_mode),
             s_pins.drive_cap);
    return true;
}

/* Switch the scanout program. Returns the 320d frame buffer (the
 * caller's new logical/display buffer), or NULL when the 320d path is
 * unavailable — the caller then falls back to set_scanout_320 with its
 * own buffer on the 640 panel. */
uint8_t * vga_lcdcam_s3_enter_320(void) {
    if (!s_panel) return NULL;
    if (s_panel_kind == 1) return s_fb320;
    /* PSRAM-fb native scanout is disabled: a heavy CPU-PSRAM workload
     * (engine game) alongside the EDMA fb scan wedged the chip hard
     * enough to need a power cycle (console-only use was fine). Until
     * that is understood, only the boot-reserved internal buffer may
     * back the native panel; everything else takes the present
     * fallback. The PSRAM path (fb_in_psram + live_sync) stays wired
     * for the investigation. */
    if (!vga320d_reserved_fb) return NULL;
    s_src = NULL;
    esp_lcd_panel_del(s_panel); /* frees the 640 panel's frame buffer */
    s_panel = NULL;
    s_fb = NULL;
    if (!vga_build_320d_panel()) {
        /* Restore the console panel so the display isn't left dead. */
        vga_build_640_panel();
        return NULL;
    }
    return s_fb320;
}

/* True when the scanout reads `buf` directly (native 320d panel):
 * writes are on screen with no present step. */
bool vga_lcdcam_s3_scanout_is_live(const uint8_t * buf) {
    return buf && s_panel_kind == 1 && buf == s_fb320;
}

/* Push CPU writes to the live 320d frame buffer out to the scanout.
 * Rows are in 320x240 space. With the boot-reserved internal buffer
 * this is free (internal SRAM is DMA-coherent); a PSRAM fb is behind
 * the write-back cache, so the DMA reader sees stale data until the
 * dirty lines are written back. */
void vga_lcdcam_s3_live_sync(int y1, int y2) {
    if (s_panel_kind != 1 || !s_fb320 || !s_fb320_psram) return;
    if (y1 < 0) y1 = 0;
    if (y2 >= VGA_LCDCAM_VRES / 2) y2 = VGA_LCDCAM_VRES / 2 - 1;
    if (y1 > y2) return;
    esp_cache_msync(s_fb320 + (size_t)y1 * (VGA_LCDCAM_HRES / 2),
                    (size_t)(y2 - y1 + 1) * (VGA_LCDCAM_HRES / 2),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

bool vga_lcdcam_s3_enter_640(void) {
    if (!s_panel) return false;
    if (s_panel_kind == 0) return true;
    s_src = NULL;
    s_fb320 = NULL;
    esp_lcd_panel_del(s_panel);
    s_panel = NULL;
    return vga_build_640_panel();
}

/* The bounce-buffer refill is a CPU memcpy from PSRAM inside the LCD
 * ISR, on the core that created the panel. It must stay on core 0 with
 * the heavy PSRAM writers (boot's 6 MB heap clear, game drawing): an
 * ISR with a ~508 us refill deadline can preempt same-core aggressors,
 * but moved to core 1 it contends cross-core for PSRAM with no way to
 * preempt — boot's heap clear alone starved it into an interrupt-WDT
 * bootloop. */
bool vga_lcdcam_s3_init(const vga_lcdcam_pins_t * pins, uint8_t ** fb_out) {
    return vga_lcdcam_s3_init_impl(pins, fb_out);
}

uint8_t * vga_lcdcam_s3_framebuffer(void) {
    return s_fb;
}

/* Block until the next frame-edge (start of vertical blanking). Writes
 * to the frame buffer issued top-down right after this stay ahead of
 * the scanout reader for the whole following frame. The pre-drain turns
 * a possibly-stale pending signal into a true edge wait; the timeout
 * (two frame times) keeps callers alive if the panel ever stops. */
void vga_lcdcam_s3_wait_vsync(void) {
    if (!s_panel || !s_frame_edge_sem) return;
    xSemaphoreTake(s_frame_edge_sem, 0);
    xSemaphoreTake(s_frame_edge_sem, pdMS_TO_TICKS(40));
}

bool vga_lcdcam_s3_active(void) {
    /* Either panel kind counts: in native 320d mode the 640 frame
     * buffer is gone but the peripheral is very much running. */
    return s_panel != NULL && (s_fb != NULL || s_fb320 != NULL);
}

void vga_lcdcam_s3_flush_region(int x1, int y1, int x2, int y2) {
    if (!s_fb) return;
    if (s_cpu_bounce_scanout) return;
    if (s_panel_kind == 1) return;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= VGA_LCDCAM_HRES) x2 = VGA_LCDCAM_HRES - 1;
    if (y2 >= VGA_LCDCAM_VRES) y2 = VGA_LCDCAM_VRES - 1;
    if (x1 > x2 || y1 > y2) return;

    const size_t width = (size_t)(x2 - x1 + 1);
    if (x1 == 0 && width == VGA_LCDCAM_HRES) {
        const size_t offset = (size_t)y1 * VGA_LCDCAM_HRES;
        const size_t bytes = (size_t)(y2 - y1 + 1) * VGA_LCDCAM_HRES;
        esp_cache_msync(s_fb + offset, bytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    } else {
        for (int y = y1; y <= y2; y++) {
            const size_t offset = (size_t)y * VGA_LCDCAM_HRES + (size_t)x1;
            esp_cache_msync(s_fb + offset, width,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        }
    }
}

void vga_lcdcam_s3_flush_all(void) {
    vga_lcdcam_s3_flush_region(0, 0, VGA_LCDCAM_HRES - 1, VGA_LCDCAM_VRES - 1);
}

void vga_lcdcam_s3_clear(uint8_t colour) {
    if (!s_fb) return;
    memset(s_fb, colour, VGA_LCDCAM_HRES * VGA_LCDCAM_VRES);
    vga_lcdcam_s3_flush_all();
}

void vga_lcdcam_s3_present_rgb332_2x(const uint8_t * src, int src_w, int src_h,
                                     int src_stride, int x1, int y1, int x2, int y2) {
    if (src == s_src) { /* live scanout source — just push the cache */
        vga_lcdcam_s3_live_sync(y1, y2);
        return;
    }
    if (!s_fb || !src || src_w <= 0 || src_h <= 0 || src_stride < src_w) return;
    const int out_w = src_w * 2;
    const int out_h = src_h * 2;
    if (out_w > VGA_LCDCAM_HRES || out_h > VGA_LCDCAM_VRES) return;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= src_w) x2 = src_w - 1;
    if (y2 >= src_h) y2 = src_h - 1;
    if (x1 > x2 || y1 > y2) return;

    const int xoff = (VGA_LCDCAM_HRES - out_w) / 2;
    const int yoff = (VGA_LCDCAM_VRES - out_h) / 2;
    for (int y = y1; y <= y2; y++) {
        const uint8_t * s = src + (size_t)y * src_stride + x1;
        uint8_t * d0 = s_fb + (size_t)(yoff + y * 2) * VGA_LCDCAM_HRES + xoff + x1 * 2;
        uint8_t * d1 = d0 + VGA_LCDCAM_HRES;
        for (int x = x1; x <= x2; x++) {
            uint8_t c = *s++;
            *d0++ = c;
            *d0++ = c;
            *d1++ = c;
            *d1++ = c;
        }
    }
    vga_lcdcam_s3_flush_region(xoff + x1 * 2, yoff + y1 * 2,
                               xoff + x2 * 2 + 1, yoff + y2 * 2 + 1);
}

static uint8_t rgb332_dither3(uint8_t c, int x, int y) {
    static const uint8_t bayer4[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5},
    };
    const uint8_t threshold = bayer4[y & 3][x & 3];
    const uint8_t r = (c >> 5) & 0x07;
    const uint8_t g = (c >> 2) & 0x07;
    const uint8_t b = c & 0x03;
    const uint8_t rv = r == 7 ? 16 : (uint8_t)(r * 2);
    const uint8_t gv = g == 7 ? 16 : (uint8_t)(g * 2);
    const uint8_t bv = b == 3 ? 16 : (uint8_t)(b * 4);
    uint8_t out = 0;
    if (rv > threshold) out |= 0x80;
    if (gv > threshold) out |= 0x10;
    if (bv > threshold) out |= 0x02;
    return out;
}

void vga_lcdcam_s3_present_rgb332_2x_dither3(const uint8_t * src, int src_w, int src_h,
                                             int src_stride, int x1, int y1, int x2, int y2) {
    if (src == s_src) { /* live scanout source — just push the cache */
        vga_lcdcam_s3_live_sync(y1, y2);
        return;
    }
    if (!s_fb || !src || src_w <= 0 || src_h <= 0 || src_stride < src_w) return;
    const int out_w = src_w * 2;
    const int out_h = src_h * 2;
    if (out_w > VGA_LCDCAM_HRES || out_h > VGA_LCDCAM_VRES) return;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= src_w) x2 = src_w - 1;
    if (y2 >= src_h) y2 = src_h - 1;
    if (x1 > x2 || y1 > y2) return;

    const int xoff = (VGA_LCDCAM_HRES - out_w) / 2;
    const int yoff = (VGA_LCDCAM_VRES - out_h) / 2;
    for (int y = y1; y <= y2; y++) {
        const uint8_t * s = src + (size_t)y * src_stride + x1;
        for (int x = x1; x <= x2; x++) {
            const uint8_t c = *s++;
            const int px = xoff + x * 2;
            const int py = yoff + y * 2;
            uint8_t * d0 = s_fb + (size_t)py * VGA_LCDCAM_HRES + px;
            uint8_t * d1 = d0 + VGA_LCDCAM_HRES;
            d0[0] = rgb332_dither3(c, px, py);
            d0[1] = rgb332_dither3(c, px + 1, py);
            d1[0] = rgb332_dither3(c, px, py + 1);
            d1[1] = rgb332_dither3(c, px + 1, py + 1);
        }
    }
    vga_lcdcam_s3_flush_region(xoff + x1 * 2, yoff + y1 * 2,
                               xoff + x2 * 2 + 1, yoff + y2 * 2 + 1);
}
