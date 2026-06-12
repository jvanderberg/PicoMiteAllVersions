/*
 * esp32_cyd_vga_text.c — char-cell VGA text console (80x40, 8x12 font)
 * over the I2S scanout driver.
 *
 * Three byte planes (character, foreground pixel byte, background pixel
 * byte) hold the screen; the scanout line-fill ISR expands one glyph row
 * per scanline with a nibble->mask LUT, so cost is ~2 masked word writes
 * per cell. The fg/bg planes store ready-to-emit RGB222 pixel bytes (sync
 * bits included), so the ISR does no palette work.
 *
 * Console bytes arrive through esp32_vga_console_putc() (hooked into
 * putConsole) and pass a small VT100/ANSI interpreter covering what
 *MMBasic's prompt, CLS, and colour handling actually emit: CR LF BS TAB,
 * CSI m (SGR reset / 24-bit fg/bg), CSI H/f/A/B/C/D cursor motion,
 * CSI J/K clears, and the ?25 cursor-visibility toggles.
 *
 * The glyph bitmaps are copied out of the flash-resident MMBasic font at
 * start: the fill ISR runs from IRAM and must not touch flash-mapped data
 * while a LittleFS write has the cache disabled.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "shared/gfx/gfx_console_shared.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"

#include "vga_i2s_esp32.h"

#define VGATXT_FONT_W 8
#define VGATXT_FONT_H 12
#define VGATXT_COLS (VGA_I2S_HRES / VGATXT_FONT_W) /* 80 */
#define VGATXT_ROWS (VGA_I2S_VRES / VGATXT_FONT_H) /* 40 */
#define VGATXT_CELLS (VGATXT_COLS * VGATXT_ROWS)

typedef struct {
    uint8_t * ch;   /* character codes */
    uint8_t * fg;   /* foreground pixel byte per cell */
    uint8_t * bg;   /* background pixel byte per cell */
    uint8_t * at;   /* per-cell attributes (VGATXT_ATTR_*) */
    uint8_t * font; /* glyph rows, 12 bytes per char, index 0 = first_char */
    uint8_t first_char;
    uint8_t char_count;
    int cx, cy;
    uint8_t cur_fg, cur_bg, cur_attr;
    /* ANSI escape state. */
    int esc; /* 0 none, 1 saw ESC, 2 in CSI */
    int parm[8];
    int nparm;
    bool priv;
    bool cursor_enabled;
    bool cursor_requested;
} vgatxt_state_t;

static vgatxt_state_t s_txt;
static volatile bool s_txt_active;

/* Nibble -> 4-pixel byte mask, in scanout (swizzled) byte order: entry
 * bit3..bit0 = pixels x..x+3, expanded to 0xFF per lit pixel. Lives in
 * DRAM (not flash .rodata): the fill ISR reads it. */
DRAM_ATTR const uint32_t esp32_vga_mask4[16] = {
    VGA_I2S_PACK4(0x00, 0x00, 0x00, 0x00),
    VGA_I2S_PACK4(0x00, 0x00, 0x00, 0xFF),
    VGA_I2S_PACK4(0x00, 0x00, 0xFF, 0x00),
    VGA_I2S_PACK4(0x00, 0x00, 0xFF, 0xFF),
    VGA_I2S_PACK4(0x00, 0xFF, 0x00, 0x00),
    VGA_I2S_PACK4(0x00, 0xFF, 0x00, 0xFF),
    VGA_I2S_PACK4(0x00, 0xFF, 0xFF, 0x00),
    VGA_I2S_PACK4(0x00, 0xFF, 0xFF, 0xFF),
    VGA_I2S_PACK4(0xFF, 0x00, 0x00, 0x00),
    VGA_I2S_PACK4(0xFF, 0x00, 0x00, 0xFF),
    VGA_I2S_PACK4(0xFF, 0x00, 0xFF, 0x00),
    VGA_I2S_PACK4(0xFF, 0x00, 0xFF, 0xFF),
    VGA_I2S_PACK4(0xFF, 0xFF, 0x00, 0x00),
    VGA_I2S_PACK4(0xFF, 0xFF, 0x00, 0xFF),
    VGA_I2S_PACK4(0xFF, 0xFF, 0xFF, 0x00),
    VGA_I2S_PACK4(0xFF, 0xFF, 0xFF, 0xFF),
};

#define VGATXT_DEFAULT_FG VGA_I2S_RGB222(3, 3, 3)
#define VGATXT_DEFAULT_BG VGA_I2S_RGB222(0, 0, 0)

/* The editor draws its status separator as SGR 4 underline across a row
 * of spaces; underline renders as the glyph cell's bottom row in fg. */
#define VGATXT_ATTR_UNDERLINE 0x01

/* ---- scanout fill (ISR context) ---- */

static void IRAM_ATTR vgatxt_fill(int y, uint8_t * dst, void * ctx) {
    vgatxt_state_t * st = (vgatxt_state_t *)ctx;
    int row = y / VGATXT_FONT_H;
    int grow = y - row * VGATXT_FONT_H;
    int base = row * VGATXT_COLS;
    const uint8_t * ch = st->ch + base;
    const uint8_t * fg = st->fg + base;
    const uint8_t * bg = st->bg + base;
    uint32_t * out = (uint32_t *)dst;
    const uint8_t * at = st->at + base;
    const bool cursor_row = st->cursor_enabled && st->cursor_requested &&
                            CursorTimer <= CURSOR_ON && row == st->cy &&
                            grow >= VGATXT_FONT_H - 2;
    for (int col = 0; col < VGATXT_COLS; col++) {
        unsigned c = ch[col];
        unsigned bits = 0;
        if (c >= st->first_char && c < (unsigned)(st->first_char + st->char_count))
            bits = st->font[(c - st->first_char) * VGATXT_FONT_H + grow];
        if ((at[col] & VGATXT_ATTR_UNDERLINE) && grow == VGATXT_FONT_H - 1)
            bits = 0xFF;
        if (cursor_row && col == st->cx) bits = 0xFF;
        uint32_t fgw = fg[col] * 0x01010101u;
        uint32_t bgw = bg[col] * 0x01010101u;
        uint32_t m = esp32_vga_mask4[bits >> 4];
        *out++ = (fgw & m) | (bgw & ~m);
        m = esp32_vga_mask4[bits & 0x0F];
        *out++ = (fgw & m) | (bgw & ~m);
    }
}

/* ---- terminal operations (task context) ---- */

static void vgatxt_clear_cells(int from, int count) {
    memset(s_txt.ch + from, ' ', count);
    memset(s_txt.fg + from, s_txt.cur_fg, count);
    memset(s_txt.bg + from, s_txt.cur_bg, count);
    memset(s_txt.at + from, 0, count);
}

/* Reverse scroll: shift everything down one row, blank the top (VT100
 * Reverse Index at the top row — the editor scrolls the view this way). */
static void vgatxt_scroll_down(void) {
    int keep = VGATXT_CELLS - VGATXT_COLS;
    memmove(s_txt.ch + VGATXT_COLS, s_txt.ch, keep);
    memmove(s_txt.fg + VGATXT_COLS, s_txt.fg, keep);
    memmove(s_txt.bg + VGATXT_COLS, s_txt.bg, keep);
    memmove(s_txt.at + VGATXT_COLS, s_txt.at, keep);
    vgatxt_clear_cells(0, VGATXT_COLS);
}

static void vgatxt_scroll(void) {
    int keep = VGATXT_CELLS - VGATXT_COLS;
    memmove(s_txt.ch, s_txt.ch + VGATXT_COLS, keep);
    memmove(s_txt.fg, s_txt.fg + VGATXT_COLS, keep);
    memmove(s_txt.bg, s_txt.bg + VGATXT_COLS, keep);
    memmove(s_txt.at, s_txt.at + VGATXT_COLS, keep);
    vgatxt_clear_cells(keep, VGATXT_COLS);
}

static void vgatxt_newline(void) {
    s_txt.cx = 0;
    if (++s_txt.cy >= VGATXT_ROWS) {
        s_txt.cy = VGATXT_ROWS - 1;
        vgatxt_scroll();
    }
}

static void vgatxt_put_glyph(uint8_t c) {
    if (s_txt.cx >= VGATXT_COLS) vgatxt_newline();
    int i = s_txt.cy * VGATXT_COLS + s_txt.cx;
    s_txt.ch[i] = c;
    s_txt.fg[i] = s_txt.cur_fg;
    s_txt.bg[i] = s_txt.cur_bg;
    s_txt.at[i] = s_txt.cur_attr;
    s_txt.cx++;
}

static uint8_t vgatxt_rgb888(int r, int g, int b) {
    return VGA_I2S_RGB222(r >> 6, g >> 6, b >> 6);
}

/* Classic ANSI 8-colour palette as RGB222 pixel bytes: index order
 * black, red, green, yellow, blue, magenta, cyan, white. The editor's
 * COLOURCODE highlighting uses these (SGR 30..37). */
static uint8_t vgatxt_ansi(int idx, int bright) {
    int v = bright ? 3 : 2;
    return VGA_I2S_RGB222((idx & 1) ? v : 0, (idx & 2) ? v : 0,
                          (idx & 4) ? v : 0);
}

static void vgatxt_sgr(void) {
    for (int i = 0; i < s_txt.nparm; i++) {
        int p = s_txt.parm[i];
        if (p == 0) {
            s_txt.cur_fg = VGATXT_DEFAULT_FG;
            s_txt.cur_bg = VGATXT_DEFAULT_BG;
            s_txt.cur_attr = 0;
        } else if (p == 4) {
            s_txt.cur_attr |= VGATXT_ATTR_UNDERLINE;
        } else if (p == 24) {
            s_txt.cur_attr &= (uint8_t)~VGATXT_ATTR_UNDERLINE;
        } else if (p >= 30 && p <= 37) {
            s_txt.cur_fg = vgatxt_ansi(p - 30, 0);
        } else if (p >= 90 && p <= 97) {
            s_txt.cur_fg = vgatxt_ansi(p - 90, 1);
        } else if (p == 39) {
            s_txt.cur_fg = VGATXT_DEFAULT_FG;
        } else if (p >= 40 && p <= 47) {
            s_txt.cur_bg = vgatxt_ansi(p - 40, 0);
        } else if (p >= 100 && p <= 107) {
            s_txt.cur_bg = vgatxt_ansi(p - 100, 1);
        } else if (p == 49) {
            s_txt.cur_bg = VGATXT_DEFAULT_BG;
        } else if ((p == 38 || p == 48) && i + 4 < s_txt.nparm &&
                   s_txt.parm[i + 1] == 2) {
            uint8_t px = vgatxt_rgb888(s_txt.parm[i + 2], s_txt.parm[i + 3],
                                       s_txt.parm[i + 4]);
            if (p == 38)
                s_txt.cur_fg = px;
            else
                s_txt.cur_bg = px;
            i += 4;
        }
    }
}

static void vgatxt_csi(uint8_t final) {
    int p0 = s_txt.nparm > 0 ? s_txt.parm[0] : 0;
    int p1 = s_txt.nparm > 1 ? s_txt.parm[1] : 0;
    if (s_txt.priv) {
        if (p0 == 25) {
            if (final == 'h') s_txt.cursor_enabled = true;
            if (final == 'l') s_txt.cursor_enabled = false;
        }
        return;
    }
    switch (final) {
    case 'm':
        if (s_txt.nparm == 0) {
            s_txt.cur_fg = VGATXT_DEFAULT_FG;
            s_txt.cur_bg = VGATXT_DEFAULT_BG;
        } else
            vgatxt_sgr();
        break;
    case 'H':
    case 'f':
        s_txt.cy = p0 > 0 ? p0 - 1 : 0;
        s_txt.cx = p1 > 0 ? p1 - 1 : 0;
        if (s_txt.cy >= VGATXT_ROWS) s_txt.cy = VGATXT_ROWS - 1;
        if (s_txt.cx >= VGATXT_COLS) s_txt.cx = VGATXT_COLS - 1;
        break;
    case 'A':
        s_txt.cy -= (p0 ? p0 : 1);
        if (s_txt.cy < 0) s_txt.cy = 0;
        break;
    case 'B':
        s_txt.cy += (p0 ? p0 : 1);
        if (s_txt.cy >= VGATXT_ROWS) s_txt.cy = VGATXT_ROWS - 1;
        break;
    case 'C':
        s_txt.cx += (p0 ? p0 : 1);
        if (s_txt.cx >= VGATXT_COLS) s_txt.cx = VGATXT_COLS - 1;
        break;
    case 'D':
        s_txt.cx -= (p0 ? p0 : 1);
        if (s_txt.cx < 0) s_txt.cx = 0;
        break;
    case 'J':
        if (p0 == 2) {
            vgatxt_clear_cells(0, VGATXT_CELLS);
            s_txt.cx = 0;
            s_txt.cy = 0;
        } else { /* 0: cursor to end */
            int i = s_txt.cy * VGATXT_COLS + s_txt.cx;
            vgatxt_clear_cells(i, VGATXT_CELLS - i);
        }
        break;
    case 'K': {
        int i = s_txt.cy * VGATXT_COLS + s_txt.cx;
        vgatxt_clear_cells(i, VGATXT_COLS - s_txt.cx);
        break;
    }
    default:
        break;
    }
}

/* Console output hook: called from putConsole for every console byte. */
void esp32_vga_console_putc(int ic) {
    if (!s_txt_active) return;
    uint8_t c = (uint8_t)ic;

    if (s_txt.esc == 1) {
        if (c == '[') {
            s_txt.esc = 2;
            s_txt.nparm = 0;
            s_txt.priv = false;
            memset(s_txt.parm, 0, sizeof(s_txt.parm));
            return;
        }
        s_txt.esc = 0;
        if (c == 'M') { /* Reverse Index: up, scrolling at the top row */
            if (s_txt.cy > 0)
                s_txt.cy--;
            else
                vgatxt_scroll_down();
        } else if (c == 'D') { /* Index: down, scrolling at the bottom */
            if (s_txt.cy < VGATXT_ROWS - 1)
                s_txt.cy++;
            else
                vgatxt_scroll();
        }
        return;
    }
    if (s_txt.esc == 2) {
        if (c >= '0' && c <= '9') {
            if (s_txt.nparm == 0) s_txt.nparm = 1;
            int * p = &s_txt.parm[s_txt.nparm - 1];
            *p = *p * 10 + (c - '0');
        } else if (c == ';') {
            if (s_txt.nparm < 8) s_txt.nparm++;
            if (s_txt.nparm == 1) s_txt.nparm = 2; /* leading ';' */
        } else if (c == '?') {
            s_txt.priv = true;
        } else {
            vgatxt_csi(c);
            s_txt.esc = 0;
        }
        return;
    }

    switch (c) {
    case 0x1b:
        s_txt.esc = 1;
        break;
    case '\r':
        s_txt.cx = 0;
        break;
    case '\n':
        vgatxt_newline();
        break;
    case '\b':
        if (s_txt.cx > 0) s_txt.cx--;
        break;
    case '\t':
        do {
            vgatxt_put_glyph(' ');
        } while (s_txt.cx & 1);
        break;
    default:
        if (c >= 0x20) vgatxt_put_glyph(c);
        break;
    }
}

/* ---- start/stop ---- */

static void vgatxt_free(void) {
    heap_caps_free(s_txt.ch);
    heap_caps_free(s_txt.fg);
    heap_caps_free(s_txt.bg);
    heap_caps_free(s_txt.at);
    heap_caps_free(s_txt.font);
    memset(&s_txt, 0, sizeof(s_txt));
}

static int vgatxt_show_cursor(int show) {
    if (!s_txt_active) return 0;
    s_txt.cursor_requested = show != 0;
    return 1;
}

int esp32_vga_text_start(const int8_t data_gpio[8]) {
    if (s_txt_active) return 1;

    /* MMBasic font 1 (8x12): copy the glyphs out of flash for the ISR. */
    const unsigned char * fp = FontTable[0];
    if (!fp || fp[0] != VGATXT_FONT_W || fp[1] != VGATXT_FONT_H) return 0;

    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    s_txt.ch = heap_caps_malloc(VGATXT_CELLS, caps);
    s_txt.fg = heap_caps_malloc(VGATXT_CELLS, caps);
    s_txt.bg = heap_caps_malloc(VGATXT_CELLS, caps);
    s_txt.at = heap_caps_malloc(VGATXT_CELLS, caps);
    s_txt.font = heap_caps_malloc((size_t)fp[3] * VGATXT_FONT_H, caps);
    if (!s_txt.ch || !s_txt.fg || !s_txt.bg || !s_txt.at || !s_txt.font) {
        vgatxt_free();
        return 0;
    }
    s_txt.first_char = fp[2];
    s_txt.char_count = fp[3];
    memcpy(s_txt.font, fp + 4, (size_t)fp[3] * VGATXT_FONT_H);

    s_txt.cur_fg = VGATXT_DEFAULT_FG;
    s_txt.cur_bg = VGATXT_DEFAULT_BG;
    s_txt.cur_attr = 0;
    s_txt.cursor_enabled = true;
    s_txt.cursor_requested = false;
    vgatxt_clear_cells(0, VGATXT_CELLS);
    s_txt.cx = 0;
    s_txt.cy = 0;
    s_txt.esc = 0;

    if (!vga_i2s_start(data_gpio, vgatxt_fill, &s_txt)) {
        vgatxt_free();
        return 0;
    }
    s_txt_active = true;
    gfx_console_set_cursor_hook(vgatxt_show_cursor);
    return 1;
}

void esp32_vga_text_stop(void) {
    if (!s_txt_active) return;
    gfx_console_set_cursor_hook(NULL);
    s_txt_active = false;
    vga_i2s_stop();
    vgatxt_free();
}

bool esp32_vga_text_active(void) {
    return s_txt_active;
}

void esp32_vga_text_cursor_hook_enable(bool enable) {
    if (!s_txt_active) return;
    gfx_console_set_cursor_hook(enable ? vgatxt_show_cursor : NULL);
}

/* Re-point the scanout at the text console (graphics mode -> MODE 3).
 * The cell planes were kept, so the console resumes with its history. */
void esp32_vga_text_resume_fill(void) {
    if (s_txt_active) {
        vga_i2s_set_fill(vgatxt_fill, &s_txt);
        esp32_vga_text_cursor_hook_enable(true);
    }
}
