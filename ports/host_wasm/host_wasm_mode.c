/*
 * host_wasm_mode.c — BASIC `MODE N` for the WASM browser port.
 *
 * BASIC source can do `MODE 2` to switch to the resolution AND colour
 * depth the user assigned to mode 2 in the config dialog.  The mapping is
 * pushed in from JS at boot via wasm_set_mode_resolution(); see
 * ports/host_wasm/web/app.mjs and ports/host_wasm/web/worker.mjs.
 * Mid-program switches reallocate the framebuffer in place (host_fb_resize)
 * and bump host_fb_config_generation so the JS canvas can resize and
 * re-read the colour depth on the next rAF tick.
 *
 * The host framebuffer is always 24-bit; "colour depth" sets gui_colour_depth
 * (so GUI controls drop the gloss gradient below 8 bpp) and is reported to JS,
 * which quantises the canvas in its shader to preview the low-depth look.
 *
 * Modes 1-5 mirror the legacy VGA family.  Unconfigured slots raise
 * "Mode N not configured".
 */

#include "MMBasic.h"
#include "Commands.h"
#include "../host_native/host_fb.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

extern int gui_colour_depth;

#define HOST_WASM_MODE_MAX 5

static int mode_w[HOST_WASM_MODE_MAX + 1] = {0}; /* 1-indexed; [0] unused */
static int mode_h[HOST_WASM_MODE_MAX + 1] = {0};
static int mode_depth[HOST_WASM_MODE_MAX + 1] = {0}; /* bits; 0/unset => 24 */

/*
 * JS -> wasm: set the resolution + colour depth assigned to mode `mode`.
 * Called once per assigned mode at boot, before BASIC starts running, and
 * again whenever the config dialog changes (no reboot needed).
 */
EMSCRIPTEN_KEEPALIVE
void wasm_set_mode_resolution(int mode, int w, int h, int depth) {
    if (mode < 1 || mode > HOST_WASM_MODE_MAX) return;
    if (w <= 0 || h <= 0) {
        mode_w[mode] = 0;
        mode_h[mode] = 0;
        mode_depth[mode] = 0;
        return;
    }
    mode_w[mode] = w;
    mode_h[mode] = h;
    mode_depth[mode] = (depth >= 1) ? depth : 24;
}

/* Current framebuffer colour depth, read by JS after a resize to drive the
 * canvas-quantising shader. */
EMSCRIPTEN_KEEPALIVE
int wasm_current_colour_depth(void) {
    return gui_colour_depth;
}

int host_wasm_mode_lookup(int mode, int * w, int * h) {
    if (mode < 1 || mode > HOST_WASM_MODE_MAX) return 0;
    if (mode_w[mode] <= 0 || mode_h[mode] <= 0) return 0;
    if (w) *w = mode_w[mode];
    if (h) *h = mode_h[mode];
    return 1;
}

static int host_wasm_mode_depth(int mode) {
    if (mode < 1 || mode > HOST_WASM_MODE_MAX || mode_depth[mode] <= 0) return 24;
    return mode_depth[mode];
}

void cmd_mode(void) {
    int mode = getint(cmdline, 1, HOST_WASM_MODE_MAX);
    int w, h;
    if (!host_wasm_mode_lookup(mode, &w, &h))
        error("Mode % not configured", mode);
    gui_colour_depth = host_wasm_mode_depth(mode);
    host_fb_resize(w, h);
    host_fb_config_generation++; /* ensure JS re-reads depth even if the size didn't change */
}
