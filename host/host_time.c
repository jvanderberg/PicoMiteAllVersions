/*
 * host_time.c -- Monotonic time + msec-tick synthesizer for the host build.
 *
 * Moved out of host_stubs_legacy.c as part of the Host HAL refactor (Phase 1).
 * No behavior change — same functions, same semantics.
 *
 * The msec synthesizer (host_sync_msec_timer_value) updates MMBasic's
 * millisecond-granularity counters from the monotonic clock so that code
 * which polls mSecTimer / CursorTimer / the PAUSE timer sees forward
 * progress without a hardware 1ms IRQ. In --sim mode, a separate tick
 * thread in host_sim_server also bumps these counters every ms, so the
 * synthesizer is redundant but harmless there.
 */

#include <errno.h>
#include <time.h>

#ifdef MMBASIC_WASM
#include <emscripten.h>
#endif

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "host_time.h"

/* mSecTimer / CursorTimer are defined in host_stubs_legacy.c; referenced
 * as externs via Hardware_Includes.h. CURSOR_OFF / CURSOR_ON come from
 * the same chain. */

static void host_sync_msec_timer_value(uint64_t now_us) {
    mSecTimer = (long long)(now_us / 1000ULL);
    /* CursorTimer ticks at 1kHz on device via the timer IRQ in
     * PicoMite.c:884. On host there's no IRQ — synthesize it from the
     * monotonic clock so ShowCursor's blink math works. */
    CursorTimer = (int)((now_us / 1000ULL) % (CURSOR_OFF + CURSOR_ON));
}

uint64_t host_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

void host_sync_msec_timer(void) {
    host_sync_msec_timer_value(host_now_us());
}

uint64_t host_time_us_64(void) {
    uint64_t now = host_now_us();
    host_sync_msec_timer_value(now);
    return now;
}

#ifdef MMBASIC_WASM
/* Shared with host_runtime.c's wasm_yield_if_due: any path that actually
 * yields to JS (host_sleep_us, FASTGFX SYNC's host_sleep_us, PAUSE,
 * emscripten_sleep(0) in wasm_yield_if_due) stamps this so the periodic
 * yield hook skips redundant unwinds. Games like pico_blocks that call
 * FASTGFX SYNC every 20 ms never pay for a second yield on CheckAbort. */
uint64_t wasm_last_yield_us = 0;
#endif

void host_sleep_us(uint64_t us) {
    if (us == 0) {
        host_sync_msec_timer();
        return;
    }

#ifdef MMBASIC_WASM
    /* ASYNCIFY unwinds the C stack to JS, runs the event loop, then
     * resumes. The granularity floor is the browser event-loop latency
     * (often 4 ms on throttled tabs), so sub-millisecond PAUSE values
     * behave like 1 ms — noted in the web-host plan's "timing drift"
     * risk section. */
    unsigned ms = (unsigned)((us + 999ULL) / 1000ULL);
    if (ms == 0) ms = 1;
    emscripten_sleep(ms);
    /* Tell the periodic yield hook we just unwound to JS, so it doesn't
     * fire again for another 16 ms. */
    wasm_last_yield_us = host_now_us();
#else
    struct timespec req;
    req.tv_sec = (time_t)(us / 1000000ULL);
    req.tv_nsec = (long)((us % 1000000ULL) * 1000ULL);
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
#endif
    host_sync_msec_timer();
}
