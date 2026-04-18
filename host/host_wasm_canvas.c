/*
 * host_wasm_canvas.c -- JS-facing exports for the WASM build.
 *
 * JS needs a stable pointer into the front framebuffer plus its
 * dimensions so it can call ctx.putImageData once per rAF tick. The
 * framebuffer itself lives in host_fb.c (same as the native host) — all
 * this file does is surface a handful of `wasm_*` functions for
 * emscripten's EXPORTED_FUNCTIONS to export.
 *
 * Phase 1 doesn't implement a dirty-rect accumulator: JS re-blits the
 * whole framebuffer every frame, which at 320x240x32 = 300 KB/frame is
 * comfortably under 60 FPS on any reasonable browser. Dirty-rect
 * optimisation is Phase 6 work if profiling says it matters.
 */

#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

/* From host_fb.c. */
extern uint32_t *host_framebuffer;
extern int host_fb_width;
extern int host_fb_height;
extern void host_fb_ensure(void);
extern void host_sim_set_framebuffer_size(int w, int h);

/*
 * Resize the framebuffer. MUST be called BEFORE wasm_boot — the size
 * determines the backing allocation in host_fb_ensure(), which runs
 * during host_runtime_begin inside wasm_boot. Calling mid-session is
 * not supported; the JS side handles resolution changes by reloading
 * the page with a ?res=WxH query param.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_set_framebuffer_size(int w, int h) {
    host_sim_set_framebuffer_size(w, h);
}

EMSCRIPTEN_KEEPALIVE
uintptr_t wasm_framebuffer_ptr(void) {
    if (!host_framebuffer) host_fb_ensure();
    return (uintptr_t)host_framebuffer;
}

EMSCRIPTEN_KEEPALIVE
int wasm_framebuffer_width(void) {
    return host_fb_width;
}

EMSCRIPTEN_KEEPALIVE
int wasm_framebuffer_height(void) {
    return host_fb_height;
}
