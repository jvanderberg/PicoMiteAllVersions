/*
 * esp32_cyd_vga.c — classic-ESP32 VGA surface over the I2S scanout driver
 * (drivers/vga_i2s_esp32).
 *
 * Option surface (persisted in the Option.extensions[] VGA region and
 * applied at boot — esp32_vga_display_init brings the console up in
 * MODE 3 automatically when configured):
 *   OPTION VGA                          enable on the default pin map
 *   OPTION VGA r1,r0,g1,g0,b1,b0,hs,vs  enable on explicit chip GPIOs
 *                                       (MSB first per colour channel)
 *   OPTION VGA TEST [RED|GREEN|BLUE|WHITE|LADDER]
 *                                       diagnostic test cards (not saved)
 *   OPTION VGA DISABLE                  stop, release, clear saved config
 *
 * Default pin map (bus bit -> chip GPIO), matching the documented VGA666
 * wiring: B0=4 B1=5 G0=18 G1=19 R0=21 R1=22 HSync=23 VSync=15.
 *
 * Screen modes (MODE n, while OPTION VGA is active):
 *   MODE 1  640x480 1bpp graphics (esp32_cyd_vga_mode1.c primitives)
 *   MODE 2  320x240 16-colour graphics (matches VGA PicoMite MODE 2):
 *           4bpp RGB121 framebuffer driven by the shared Draw.c *16
 *           primitives; the scanout ISR maps nibbles through the
 *           colours[] palette to RGB222 while pixel/line doubling
 *   MODE 3  the char-cell text console (the OPTION VGA default)
 * MODE 1/2 require Wi-Fi off (the framebuffer and the Wi-Fi stack want
 * the same internal RAM); failures land back in the MODE 3 console.
 * DISPLAY_TYPE carries SCREENMODE1/2 in the graphics modes so do_end's
 * `DISPLAY_TYPE - SCREENMODE1 + 1` round-trips the mode number.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "vga_i2s_esp32.h"

#include "SPI-LCD.h"          /* SCREENMODEn DISPLAY_TYPE values */
#include "esp32_option_ext.h" /* Option.extensions[] VGA region */

/* Core framebuffer + draw-pointer globals (defined in Draw.c / state). */
extern short HRes, VRes, DisplayHRes, DisplayVRes;
extern short CurrentX, CurrentY;
extern short gui_font_width, gui_font_height;
extern int ScreenSize;
extern uint32_t framebuffersize;
extern unsigned char *WriteBuf, *DisplayBuf, *FrameBuf, *LayerBuf, *SecondFrame, *SecondLayer, *FRAMEBUFFER;
extern unsigned char OptionConsole;
extern volatile int DISPLAY_TYPE;
extern void (*DrawRectangle)(int x1, int y1, int x2, int y2, int c);
extern void (*DrawBitmap)(int x1, int y1, int width, int height, int scale, int fc, int bc, unsigned char * bitmap);
extern void (*ScrollLCD)(int lines);
extern void (*DrawBuffer)(int x1, int y1, int x2, int y2, unsigned char * c);
extern void (*ReadBuffer)(int x1, int y1, int x2, int y2, unsigned char * c);
extern void (*DrawBufferFast)(int x1, int y1, int x2, int y2, int blank, unsigned char * c);
extern void (*ReadBufferFast)(int x1, int y1, int x2, int y2, unsigned char * c);
extern void (*DrawPixel)(int x1, int y1, int c);
extern volatile int WIFIconnected;
extern const int colours[16]; /* RGB121 nibble -> RGB888 (Draw.c) */

extern void esp32_vga_mode1_fill(int y, uint8_t * dst, void * ctx);
extern void esp32_vga_mode1_bind_draw(void);
extern void esp32_vga_mode1_set_colours(uint8_t fg_px, uint8_t bg_px);
extern void esp32_vga_text_resume_fill(void);
extern void esp32_vga_text_cursor_hook_enable(bool enable);
extern void port_set_error_restore_console_surface_callback(void (*callback)(void));

/* Bus bits 0..7 = B0 B1 G0 G1 R0 R1 HS VS. */
static const int8_t s_vga_default_pins[8] = {4, 5, 18, 19, 21, 22, 23, 15};

/* Active pin map (bus-bit order) while VGA is up. */
static int8_t s_vga_pins[8];

/* ---- persisted configuration ----
 * Option.extensions[ESP32_OPTION_VGA_EXT_BASE + i] holds the chip GPIO
 * for bus bit i; ESP32_OPTION_VGA_VSYNC holds the enable magic. Saved
 * with SaveOptions(), so the configuration survives CPU RESTART and
 * power cycles; boot validates before applying. */
#define VGA_OPTION_MAGIC 0x56 /* 'V' */

/* User-facing argument order r1,r0,g1,g0,b1,b0,hs,vs -> bus bit. */
static const uint8_t s_vga_arg_to_bit[8] = {5, 4, 3, 2, 1, 0, 6, 7};

static int vga_gpio_output_ok(int gpio) {
    if (gpio < 0 || gpio >= HAL_PORT_GPIO_COUNT) return 0;
    int slot = (int)PINMAP[gpio];
    if (slot < 1 || slot > NBRPINS) return 0;
    if (PinDef[slot].mode & UNUSED) return 0;
    if (!(PinDef[slot].mode & DIGITAL_OUT)) return 0;
    return 1;
}

static int vga_pins_from_option(int8_t out[8]) {
    if (ESP32_OPTION_VGA_VSYNC != VGA_OPTION_MAGIC) return 0;
    for (int i = 0; i < 8; i++) {
        int gpio = ESP32_OPTION_VGA_DATA[i];
        if (!vga_gpio_output_ok(gpio)) return 0;
        out[i] = (int8_t)gpio;
    }
    return 1;
}

static void vga_store_option(const int8_t pins[8]) {
    for (int i = 0; i < 8; i++) ESP32_OPTION_VGA_DATA[i] = (unsigned char)pins[i];
    ESP32_OPTION_VGA_VSYNC = VGA_OPTION_MAGIC;
    SaveOptions();
}

static void vga_clear_option(void) {
    for (int i = 0; i < 8; i++) ESP32_OPTION_VGA_DATA[i] = 0;
    ESP32_OPTION_VGA_VSYNC = 0;
    SaveOptions();
}

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

/* ---- screen modes ---- */

#define VGA_MODE2_W 320
#define VGA_MODE2_H 240
#define VGA_MODE2_STRIDE (VGA_MODE2_W / 2) /* 4bpp packed */

static void vga_console_geometry(void);
static int vga_apply_mode(int mode, bool clear, const char ** errmsg);

static int s_vga_mode = 3;   /* valid while the text console is active */
static uint8_t * s_gfx_fb;   /* MODE 1/2 framebuffer (heap, on demand) */
static uint8_t s_lut121[16]; /* RGB121 nibble -> RGB222|sync (ISR) */

/* Display state to restore when graphics modes exit. */
static struct {
    bool saved;
    short hres, vres, dhres, dvres;
    int screensize;
    uint32_t fbsize;
    int display_type;
    int option_display_type;
    unsigned char display_console;
    short opt_width, opt_height;
    unsigned char * bufs[7];
    void (*rect)(int, int, int, int, int);
    void (*bmp)(int, int, int, int, int, int, int, unsigned char *);
    void (*scroll)(int);
    void (*dbuf)(int, int, int, int, unsigned char *);
    void (*rbuf)(int, int, int, int, unsigned char *);
    void (*dbuff)(int, int, int, int, int, unsigned char *);
    void (*rbuff)(int, int, int, int, unsigned char *);
    void (*pixel)(int, int, int);
} s_disp_save;

/* One packed source byte = two logical pixels (low nibble = even x);
 * each is pixel-doubled, so one source byte = one output word. Lines are
 * doubled by reading fb row y/2. */
static void IRAM_ATTR vga_mode2_fill(int y, uint8_t * dst, void * ctx) {
    (void)ctx;
    const uint8_t * src = s_gfx_fb + (y >> 1) * VGA_MODE2_STRIDE;
    uint32_t * out = (uint32_t *)dst;
    for (int i = 0; i < VGA_MODE2_STRIDE; i++) {
        uint8_t b = src[i];
        uint8_t a = s_lut121[b & 0x0F];
        uint8_t c = s_lut121[b >> 4];
        *out++ = VGA_I2S_PACK4(a, a, c, c);
    }
}

static uint8_t vga_px_from_rgb888(int c) {
    return VGA_I2S_RGB222(((c >> 16) & 0xFF) >> 6, ((c >> 8) & 0xFF) >> 6,
                          (c & 0xFF) >> 6);
}

/* Build the scanout LUT from the shared RGB121 palette (colours[16] maps
 * each nibble to RGB888), so the nibble bit layout stays Draw.c's
 * business. */
static void vga_build_lut121(void) {
    for (int i = 0; i < 16; i++) s_lut121[i] = vga_px_from_rgb888(colours[i]);
}

static void vga_display_state_save(void) {
    if (s_disp_save.saved) return;
    s_disp_save.saved = true;
    s_disp_save.hres = HRes;
    s_disp_save.vres = VRes;
    s_disp_save.dhres = DisplayHRes;
    s_disp_save.dvres = DisplayVRes;
    s_disp_save.screensize = ScreenSize;
    s_disp_save.fbsize = framebuffersize;
    s_disp_save.display_type = DISPLAY_TYPE;
    s_disp_save.option_display_type = Option.DISPLAY_TYPE;
    s_disp_save.display_console = Option.DISPLAY_CONSOLE;
    s_disp_save.opt_width = Option.Width;
    s_disp_save.opt_height = Option.Height;
    unsigned char * bufs[7] = {FRAMEBUFFER, WriteBuf, DisplayBuf, FrameBuf,
                               LayerBuf, SecondFrame, SecondLayer};
    memcpy(s_disp_save.bufs, bufs, sizeof(bufs));
    s_disp_save.rect = DrawRectangle;
    s_disp_save.bmp = DrawBitmap;
    s_disp_save.scroll = ScrollLCD;
    s_disp_save.dbuf = DrawBuffer;
    s_disp_save.rbuf = ReadBuffer;
    s_disp_save.dbuff = DrawBufferFast;
    s_disp_save.rbuff = ReadBufferFast;
    s_disp_save.pixel = DrawPixel;
}

static void vga_display_state_restore(void) {
    if (!s_disp_save.saved) return;
    s_disp_save.saved = false;
    HRes = s_disp_save.hres;
    VRes = s_disp_save.vres;
    DisplayHRes = s_disp_save.dhres;
    DisplayVRes = s_disp_save.dvres;
    ScreenSize = s_disp_save.screensize;
    framebuffersize = s_disp_save.fbsize;
    DISPLAY_TYPE = s_disp_save.display_type;
    Option.DISPLAY_TYPE = s_disp_save.option_display_type;
    Option.DISPLAY_CONSOLE = s_disp_save.display_console;
    Option.Width = s_disp_save.opt_width;
    Option.Height = s_disp_save.opt_height;
    FRAMEBUFFER = s_disp_save.bufs[0];
    WriteBuf = s_disp_save.bufs[1];
    DisplayBuf = s_disp_save.bufs[2];
    FrameBuf = s_disp_save.bufs[3];
    LayerBuf = s_disp_save.bufs[4];
    SecondFrame = s_disp_save.bufs[5];
    SecondLayer = s_disp_save.bufs[6];
    DrawRectangle = s_disp_save.rect;
    DrawBitmap = s_disp_save.bmp;
    ScrollLCD = s_disp_save.scroll;
    DrawBuffer = s_disp_save.dbuf;
    ReadBuffer = s_disp_save.rbuf;
    DrawBufferFast = s_disp_save.dbuff;
    ReadBufferFast = s_disp_save.rbuff;
    DrawPixel = s_disp_save.pixel;
}

static void vga_point_framebuffers_at(uint8_t * fb) {
    FRAMEBUFFER = WriteBuf = DisplayBuf = fb;
    /* FrameBuf/LayerBuf (and the Second* pair) belong to the FRAMEBUFFER
     * command — NULL until the user creates them. Aliasing them to the
     * scanout buffer makes FASTGFX's "FRAMEBUFFER is active" guard fire
     * unconditionally and FRAMEBUFFER CREATE think one already exists. */
    FrameBuf = LayerBuf = SecondFrame = SecondLayer = NULL;
}

static void vga_bind_graphics_runtime(int mode) {
    vga_point_framebuffers_at(s_gfx_fb);
    if (mode == 1) {
        HRes = DisplayHRes = VGA_I2S_HRES;
        VRes = DisplayVRes = VGA_I2S_VRES;
        ScreenSize = (VGA_I2S_HRES / 8) * VGA_I2S_VRES;
        framebuffersize = (uint32_t)ScreenSize;
        DISPLAY_TYPE = SCREENMODE1;
        Option.DISPLAY_TYPE = SCREENMODE1;
        esp32_vga_mode1_set_colours(vga_px_from_rgb888(gui_fcolour),
                                    vga_px_from_rgb888(gui_bcolour));
        esp32_vga_mode1_bind_draw();
        esp32_vga_text_cursor_hook_enable(false);
        vga_i2s_set_fill(esp32_vga_mode1_fill, NULL);
    } else {
        vga_build_lut121();
        HRes = DisplayHRes = VGA_MODE2_W;
        VRes = DisplayVRes = VGA_MODE2_H;
        ScreenSize = VGA_MODE2_STRIDE * VGA_MODE2_H;
        framebuffersize = (uint32_t)ScreenSize;
        DISPLAY_TYPE = SCREENMODE2;
        Option.DISPLAY_TYPE = SCREENMODE2;
        DrawRectangle = DrawRectangle16;
        DrawBitmap = DrawBitmap16;
        ScrollLCD = ScrollLCD16;
        DrawBuffer = DrawBuffer16;
        ReadBuffer = ReadBuffer16;
        DrawBufferFast = DrawBuffer16Fast;
        ReadBufferFast = ReadBuffer16Fast;
        DrawPixel = DrawPixel16;
        esp32_vga_text_cursor_hook_enable(false);
        vga_i2s_set_fill(vga_mode2_fill, NULL);
    }
    Option.DISPLAY_CONSOLE = 1;
    OptionConsole = 3;
    Option.Width = HRes / gui_font_width;
    Option.Height = VRes / gui_font_height;
}

static void vga_error_restore_console_surface(void) {
    if (!esp32_vga_text_active()) return;
    if (s_vga_mode == 3) {
        esp32_vga_text_resume_fill();
        vga_console_geometry();
    } else if (s_gfx_fb) {
        vga_bind_graphics_runtime(s_vga_mode);
    } else {
        const char * msg;
        (void)vga_apply_mode(3, false, &msg);
    }
}

/* Switch screen mode. Returns 0 with *errmsg set on failure (the caller
 * decides whether to error() — setmode from do_end must stay silent). */
static int vga_apply_mode(int mode, bool clear, const char ** errmsg) {
    *errmsg = NULL;
    if (!esp32_vga_text_active()) {
        *errmsg = "Use OPTION VGA first";
        return 0;
    }
    if (mode == s_vga_mode) {
        if (clear && mode != 3 && s_gfx_fb) {
            memset(s_gfx_fb, 0,
                   mode == 1 ? (VGA_I2S_HRES / 8) * VGA_I2S_VRES
                             : VGA_MODE2_STRIDE * VGA_MODE2_H);
            CurrentX = CurrentY = 0;
        }
        return 1;
    }

    if (mode == 3) {
        esp32_vga_text_resume_fill();
        vga_display_state_restore();
        vga_console_geometry();
        heap_caps_free(s_gfx_fb);
        s_gfx_fb = NULL;
        s_vga_mode = 3;
        return 1;
    }

    if (WIFIconnected) {
        *errmsg = "Graphics modes need Wi-Fi off (OPTION WIFI \"\",\"\" then reboot)";
        return 0;
    }

    size_t bytes = (mode == 1) ? (size_t)(VGA_I2S_HRES / 8) * VGA_I2S_VRES
                               : (size_t)VGA_MODE2_STRIDE * VGA_MODE2_H;
    /* Free the outgoing graphics framebuffer before allocating the new
     * one: two graphics framebuffers do not coexist in classic-ESP32
     * DRAM. The scanout must stop reading it first. */
    if (s_gfx_fb) {
        esp32_vga_text_resume_fill();
        heap_caps_free(s_gfx_fb);
        s_gfx_fb = NULL;
    }
    uint8_t * fb = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!fb) {
        ESP_LOGW("vga", "mode %d fb %u B: free=%u largest=%u", mode,
                 (unsigned)bytes,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                   MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        /* The failure floor is the text console, never a dead screen. */
        vga_display_state_restore();
        s_vga_mode = 3;
        *errmsg = "Not enough memory";
        return 0;
    }
    memset(fb, 0, bytes);
    s_gfx_fb = fb;
    vga_display_state_save();
    vga_bind_graphics_runtime(mode);
    CurrentX = CurrentY = 0;
    s_vga_mode = mode;
    return 1;
}

/* Start (or restart) the text console on `pins`. */
/* The glass terminal is 80x40; the console/editor geometry must match
 * (and must be re-asserted when graphics modes hand the screen back). */
static void vga_console_geometry(void) {
    HRes = DisplayHRes = VGA_I2S_HRES;
    VRes = DisplayVRes = VGA_I2S_VRES;
    Option.Width = 80;
    Option.Height = 40;
    /* The glass terminal mirrors the serial console byte stream through
     * the SerialConsolePutC hook — the GFX display console must be off.
     * These fields arrive stale from flash when options were saved while
     * a graphics mode was up: with no framebuffer this boot, DisplayPutC
     * would run against HRes=0 (its right-margin wrap then loops forever
     * on tab expansion) and ScrollLCD would scribble on freed memory. */
    Option.DISPLAY_CONSOLE = 0;
    OptionConsole = 1;
    DISPLAY_TYPE = 0;
    Option.DISPLAY_TYPE = 0;
}

static void vga_console_start(const int8_t pins[8]) {
    port_set_error_restore_console_surface_callback(vga_error_restore_console_surface);
    if (vga_i2s_is_active() && !esp32_vga_text_active())
        vga_disable(); /* test card -> console */
    if (esp32_vga_text_active()) {
        if (memcmp(pins, s_vga_pins, sizeof(s_vga_pins)) == 0)
            return; /* already up on these pins */
        if (s_vga_mode != 3) {
            const char * msg;
            (void)vga_apply_mode(3, false, &msg);
        }
        esp32_vga_text_stop();
    }
    memcpy(s_vga_pins, pins, sizeof(s_vga_pins));
    if (!esp32_vga_text_start(s_vga_pins)) error("VGA start failed");
    vga_console_geometry();
}

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
        if (s_vga_mode != 3) {
            const char * msg;
            (void)vga_apply_mode(3, false, &msg);
        }
        esp32_vga_text_stop();
        vga_disable();
        vga_clear_option();
        return 1;
    }
    skipspace(tp);
    if (!*tp || *tp == '\'') {
        /* Bare OPTION VGA: the default pin map. */
        vga_console_start(s_vga_default_pins);
        vga_store_option(s_vga_default_pins);
        return 1;
    }
    {
        /* OPTION VGA r1,r0,g1,g0,b1,b0,hsync,vsync — chip GPIO numbers. */
        int8_t pins[8];
        getargs(&tp, 15, (unsigned char *)",");
        if (argc != 15) error("OPTION VGA r1,r0,g1,g0,b1,b0,hsync,vsync");
        for (int i = 0; i < 8; i++) {
            int gpio = (int)getinteger(argv[i * 2]);
            if (!vga_gpio_output_ok(gpio)) error("Invalid pin");
            pins[s_vga_arg_to_bit[i]] = (int8_t)gpio;
        }
        for (int i = 0; i < 8; i++)
            for (int j = i + 1; j < 8; j++)
                if (pins[i] == pins[j]) error("Duplicate pin");
        vga_console_start(pins);
        vga_store_option(pins);
        return 1;
    }
}

void esp32_vga_print_options(void) {
    if (esp32_vga_text_active()) {
        char buf[64];
        snprintf(buf, sizeof buf, "OPTION VGA %d,%d,%d,%d,%d,%d,%d,%d\r\n",
                 s_vga_pins[5], s_vga_pins[4], s_vga_pins[3], s_vga_pins[2],
                 s_vga_pins[1], s_vga_pins[0], s_vga_pins[6], s_vga_pins[7]);
        MMPrintString(buf);
    } else if (vga_i2s_is_active()) {
        MMPrintString("OPTION VGA TEST\r\n");
    }
}

/* Boot bring-up: apply the persisted configuration (called from app_main
 * after the console glue and options are loaded). The boot screen mode is
 * always the MODE 3 console — the safety floor, and the only mode that
 * coexists with a configured Wi-Fi connection. */
void esp32_vga_display_init(void) {
    port_set_error_restore_console_surface_callback(vga_error_restore_console_surface);
    int8_t pins[8];
    if (!vga_pins_from_option(pins)) return;
    memcpy(s_vga_pins, pins, sizeof(s_vga_pins));
    if (!esp32_vga_text_start(s_vga_pins)) {
        MMPrintString("VGA start failed; serial console only\r\n");
        return;
    }
    vga_console_geometry();
}

void esp32_vga_reserve_option_pins(void) {}

/* ---- legacy S3 entry points still referenced by shared sources ---- */

bool vga_lcdcam_s3_active(void) {
    return false;
}

uint8_t * vga_lcdcam_s3_framebuffer(void) {
    return NULL;
}

int esp32_packed_vga_active(void) {
    return esp32_vga_text_active() && s_vga_mode == 2 && s_gfx_fb != NULL;
}

/* Commands.c calls setmode from cmd_new/do_end with
 * DISPLAY_TYPE - SCREENMODE1 + 1; in the graphics modes that round-trips
 * our mode number, in MODE 3 the restored DISPLAY_TYPE makes it
 * out-of-range and this returns. Must never error(): error() runs
 * do_end(), so a failure here would recurse. */
void setmode(int mode, bool clear) {
    if (!esp32_vga_text_active()) return;
    if (mode < 1 || mode > 3) return;
    const char * msg;
    (void)vga_apply_mode(mode, clear, &msg);
}

void cmd_mode(void) {
    int mode = (int)getint(cmdline, 1, 3);
    const char * msg;
    if (!vga_apply_mode(mode, true, &msg)) error((char *)msg);
}
