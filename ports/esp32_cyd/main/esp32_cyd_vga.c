/*
 * esp32_cyd_vga.c — classic-ESP32 VGA surface over the I2S scanout driver
 * (drivers/vga_i2s_esp32).
 *
 * Current surface:
 *   OPTION VGA TEST     bring the scanout up on the default pin map with a
 *                       test card (colour bars, white border, moving line)
 *                       — the bring-up/diagnostic image.
 *   OPTION VGA DISABLE  stop the scanout and release pins/RAM.
 *
 * Default pin map (bus bit -> chip GPIO), matching the documented VGA666
 * wiring: B0=4 B1=5 G0=18 G1=19 R0=21 R1=22 HSync=23 VSync=15.
 *
 * The S3 port's LCD_CAM VGA surface (OPTION VGA pin lists, MODE 1/2/3
 * presentation) lands here next; the legacy S3 entry points below keep the
 * shared sources linking until then. MODE remains unsupported until the
 * presentation modes exist.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"

#include "vga_i2s_esp32.h"

/* Bus bits 0..7 = B0 B1 G0 G1 R0 R1 HS VS. */
static const int8_t s_vga_default_pins[8] = {4, 5, 18, 19, 21, 22, 23, 15};

/* ---- test card ----
 * Two pre-swizzled template lines built at start: the colour-bar line
 * (with white border columns baked in) and a solid white line. The fill
 * ISR just copies one or the other, plus a white "beam" row that walks
 * down the screen to prove the per-line refill is live. */

static uint8_t * s_test_bars;
static uint8_t * s_test_white;
static volatile int s_test_beam;

static void IRAM_ATTR vga_test_fill(int y, uint8_t * dst, void * ctx) {
    (void)ctx;
    if (y == 0) {
        int beam = s_test_beam + 1;
        if (beam >= VGA_I2S_VRES - 1) beam = 1;
        s_test_beam = beam;
    }
    const uint8_t * src =
        (y == 0 || y == VGA_I2S_VRES - 1 || y == s_test_beam) ? s_test_white
                                                              : s_test_bars;
    memcpy(dst, src, VGA_I2S_HRES);
}

static void vga_test_free_templates(void) {
    heap_caps_free(s_test_bars);
    s_test_bars = NULL;
    heap_caps_free(s_test_white);
    s_test_white = NULL;
}

static int vga_test_start(void) {
    if (vga_i2s_is_active()) return 1;
    /* 8BIT keeps the allocator out of the word-only IRAM heap region. */
    s_test_bars = heap_caps_malloc(VGA_I2S_HRES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_test_white = heap_caps_malloc(VGA_I2S_HRES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_test_bars || !s_test_white) {
        vga_test_free_templates();
        return 0;
    }

    /* 8 bars of 80 pixels: white, yellow, cyan, green, magenta, red,
     * blue, black — the classic order. */
    static const uint8_t bar_rgb[8][3] = {{3, 3, 3}, {3, 3, 0}, {0, 3, 3}, {0, 3, 0}, {3, 0, 3}, {3, 0, 0}, {0, 0, 3}, {0, 0, 0}};
    for (int x = 0; x < VGA_I2S_HRES; x++) {
        const uint8_t * c = bar_rgb[x / 80];
        uint8_t px = VGA_I2S_RGB222(c[0], c[1], c[2]);
        if (x == 0 || x == VGA_I2S_HRES - 1) px = VGA_I2S_RGB222(3, 3, 3);
        VGA_I2S_PX(s_test_bars, x, px);
        VGA_I2S_PX(s_test_white, x, VGA_I2S_RGB222(3, 3, 3));
    }
    s_test_beam = 1;

    if (!vga_i2s_start(s_vga_default_pins, vga_test_fill, NULL)) {
        vga_test_free_templates();
        return 0;
    }
    return 1;
}

static void vga_disable(void) {
    vga_i2s_stop();
    vga_test_free_templates();
}

/* ---- OPTION VGA surface ---- */

/* Repaint both template lines with one solid colour: a per-channel
 * diagnostic (OPTION VGA TEST RED/GREEN/BLUE/WHITE). */
static void vga_test_solid(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t px = VGA_I2S_RGB222(r, g, b);
    for (int x = 0; x < VGA_I2S_HRES; x++) {
        VGA_I2S_PX(s_test_bars, x, px);
        VGA_I2S_PX(s_test_white, x, px);
    }
}

/* Per-wire ladder diagnostic (OPTION VGA TEST LADDER): six vertical bands
 * showing each colour wire alone — R lsb, R msb, G lsb, G msb, B lsb,
 * B msb. Within each colour the second band must be clearly (2x) brighter
 * than the first; the three msb bands should match each other. A pair
 * that looks equal is swapped within the channel; a colour whose bands
 * are dimmer than its peers sits on weaker ladder taps. */
static void vga_test_ladder(void) {
    static const uint8_t band_rgb[6][3] = {{1, 0, 0}, {2, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0, 0, 1}, {0, 0, 2}};
    for (int x = 0; x < VGA_I2S_HRES; x++) {
        int band = x * 6 / VGA_I2S_HRES;
        const uint8_t * c = band_rgb[band];
        uint8_t px = VGA_I2S_RGB222(c[0], c[1], c[2]);
        VGA_I2S_PX(s_test_bars, x, px);
        VGA_I2S_PX(s_test_white, x, px);
    }
}

extern int esp32_vga_text_start(const int8_t data_gpio[8]);
extern void esp32_vga_text_stop(void);
extern bool esp32_vga_text_active(void);

int esp32_vga_option_setter(unsigned char * line) {
    unsigned char * tp = checkstring(line, (unsigned char *)"VGA");
    if (!tp) return 0;
    unsigned char * p;
    if ((p = checkstring(tp, (unsigned char *)"TEST"))) {
        if (esp32_vga_text_active()) error("VGA already on");
        if (!vga_test_start()) error("VGA start failed");
        skipspace(p);
        if (checkstring(p, (unsigned char *)"RED"))
            vga_test_solid(3, 0, 0);
        else if (checkstring(p, (unsigned char *)"GREEN"))
            vga_test_solid(0, 3, 0);
        else if (checkstring(p, (unsigned char *)"BLUE"))
            vga_test_solid(0, 0, 3);
        else if (checkstring(p, (unsigned char *)"WHITE"))
            vga_test_solid(3, 3, 3);
        else if (checkstring(p, (unsigned char *)"LADDER"))
            vga_test_ladder();
        return 1;
    }
    if (checkstring(tp, (unsigned char *)"DISABLE")) {
        esp32_vga_text_stop();
        vga_disable();
        return 1;
    }
    skipspace(tp);
    if (!*tp || *tp == '\'') {
        /* Bare OPTION VGA: the char-cell text console on the default
         * pin map. */
        if (vga_i2s_is_active() && !esp32_vga_text_active())
            vga_disable(); /* test card -> console */
        if (!esp32_vga_text_start(s_vga_default_pins))
            error("VGA start failed");
        return 1;
    }
    error("OPTION VGA [TEST|DISABLE]");
    return 1;
}

void esp32_vga_print_options(void) {
    if (esp32_vga_text_active())
        MMPrintString("OPTION VGA\r\n");
    else if (vga_i2s_is_active())
        MMPrintString("OPTION VGA TEST\r\n");
}

void esp32_vga_display_init(void) {}

void esp32_vga_reserve_option_pins(void) {}

/* ---- legacy S3 entry points still referenced by shared sources ---- */

bool vga_lcdcam_s3_active(void) {
    return false;
}

uint8_t * vga_lcdcam_s3_framebuffer(void) {
    return NULL;
}

/* Commands.c calls setmode from cmd_new/do_end to restore the screen mode
 * after a program ends; the VGA presentation modes do not exist yet, so
 * there is nothing to restore. It must stay a silent no-op: error() runs
 * do_end(), so a failure here would recurse. */
void setmode(int mode, bool clear) {
    (void)mode;
    (void)clear;
}

void cmd_mode(void) {
    error("MODE not supported on this port");
}
