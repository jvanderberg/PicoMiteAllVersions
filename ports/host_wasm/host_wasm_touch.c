/*
 * host_wasm_touch.c — mouse-as-touch bridge for the browser GUI controls.
 *
 * The shared GUI controls code (drivers/gui_controls/GUI.c) consumes touch
 * through three hardware seams that, on a device, live in
 * drivers/gui_touch/Touch.c (not built for WASM):
 *
 *   - TOUCH_DOWN  == !PinRead(Option.TOUCH_IRQ)  : current pen state
 *   - GetTouch(axis)                             : pen coordinate
 *   - TOUCH_GETIRQTRIS != 0                       : "a touch panel exists"
 *
 * JavaScript (app.mjs) writes a 3-int state block on pointer events, which
 * we expose via wasm_touch_state_ptr(). The browser already hands us
 * framebuffer-space coordinates, so no calibration is needed.
 *
 * To reuse the device touch machinery verbatim we translate the virtual
 * pen state onto the virtual TOUCH_IRQ pin level each tick, then let the
 * stock hal_gui_controls_timer_tick() poll TOUCH_DOWN and raise the
 * TouchDown/TouchUp edges + advance TouchTimer (it stands in for the
 * device's 1 ms timer IRQ). The interpreter thread runs ProcessTouch and
 * the redraw via hal_gui_controls_periodic() (host_runtime_poll_hook).
 *
 * The state block lives in shared (pthread) memory, so it is volatile.
 */

#include <pthread.h>
#include <stdint.h>
#include <time.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "hal/hal_pin.h"
#include "hal/hal_gui_controls.h"

/* [0]=x, [1]=y, [2]=pen-down(0/1). Written by JS via Atomics.store. */
static volatile int wasm_touch_state[3] = {0, 0, 0};

/* GUI.c gates its touch-panel paths on a non-zero TOUCH_GETIRQTRIS; on a
 * device InitTouch sets it. We always have a (virtual) panel. */
int TOUCH_GETIRQTRIS = 1;

EMSCRIPTEN_KEEPALIVE
uintptr_t wasm_touch_state_ptr(void) {
    return (uintptr_t)wasm_touch_state;
}

/* JS portable entry point (the shipping app writes the block directly via
 * Atomics on the exported pointer, but tests use this). */
EMSCRIPTEN_KEEPALIVE
void wasm_push_touch(int x, int y, int is_down) {
    wasm_touch_state[0] = x;
    wasm_touch_state[1] = y;
    wasm_touch_state[2] = is_down ? 1 : 0;
}

/* GUI.c reads the coordinate via GetTouch(GET_X_AXIS / GET_Y_AXIS), and
 * relies on it returning TOUCH_ERROR while the pen is up — e.g. the MSGBOX
 * modal waits for release with `while (GetTouch(...) != TOUCH_ERROR)`. */
int GetTouch(int axis) {
    if (!wasm_touch_state[2]) return TOUCH_ERROR;
    return (axis == GET_Y_AXIS) ? wasm_touch_state[1] : wasm_touch_state[0];
}

/* TOUCH(X / Y / DOWN / UP / REF / LASTREF / LASTX / LASTY) — mirrors the
 * device fun_touch (drivers/gui_touch/Touch.c). The host build's empty
 * fun_touch stub is compiled out when GUICONTROLS is on. */
void fun_touch(void) {
    if (checkstring(ep, (unsigned char *)"X"))
        iret = GetTouch(GET_X_AXIS);
    else if (checkstring(ep, (unsigned char *)"Y"))
        iret = GetTouch(GET_Y_AXIS);
    else if (checkstring(ep, (unsigned char *)"DOWN"))
        iret = TOUCH_DOWN;
    else if (checkstring(ep, (unsigned char *)"UP"))
        iret = !TOUCH_DOWN;
    else if (hal_gui_controls_get_touch_attr(ep, &iret))
        ; /* REF / LASTREF / LASTX / LASTY */
    else
        error("Invalid argument");
    targ = T_INT;
}

/* Mirror the virtual pen onto the virtual TOUCH_IRQ pin (active-low) so
 * TOUCH_DOWN reads correctly, then run the stock GUI timer tick. */
static void wasm_touch_pump(void) {
    hal_pin_write(PinDef[Option.TOUCH_IRQ].GPno, wasm_touch_state[2] ? false : true);
    hal_gui_controls_timer_tick();
}

/* ---- 1 ms tick: stands in for the device timer IRQ -------------------- */

static pthread_t gui_tick_thread;
static volatile int gui_tick_running = 0;

static void * gui_tick_body(void * unused) {
    (void)unused;
    struct timespec req = {.tv_sec = 0, .tv_nsec = 1000000L}; /* 1 ms */
    while (gui_tick_running) {
        nanosleep(&req, NULL);
        wasm_touch_pump();
    }
    return NULL;
}

/* Runs on the interpreter thread (host_runtime_poll_hook), so ProcessTouch
 * and any redraw it triggers happen where the rest of MMBasic draws. If the
 * 1 ms tick thread didn't start, drive the tick here too so touch still
 * works (at the poll cadence rather than a clean 1 ms). */
static void wasm_gui_poll_hook(void) {
    if (!gui_tick_running) wasm_touch_pump();
    hal_gui_controls_periodic();
}

extern void (*host_runtime_poll_hook)(void);

void wasm_gui_touch_start(void) {
    if (gui_tick_running) return;
    host_runtime_poll_hook = wasm_gui_poll_hook;
    gui_tick_running = 1;
    if (pthread_create(&gui_tick_thread, NULL, gui_tick_body, NULL) != 0) {
        gui_tick_running = 0; /* periodic still runs the pump from the poll hook */
    } else {
        pthread_detach(gui_tick_thread);
    }
}
