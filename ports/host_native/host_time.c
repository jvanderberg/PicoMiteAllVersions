/*
 * host_time.c -- Monotonic time + msec-tick synthesizer for the host build.
 *
 * The msec synthesizer (host_sync_msec_timer_value) updates MMBasic's
 * millisecond-granularity counters from the monotonic clock so that code
 * which polls mSecTimer / CursorTimer / the PAUSE timer sees forward
 * progress without a hardware 1ms IRQ. In --sim mode, a separate tick
 * thread in host_sim_server also bumps these counters every ms, so the
 * synthesizer is redundant but harmless there.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "host_time.h"

/* Set by the WEB NTP path (host_web.c / host_wasm_web.c) so DATE$/TIME$
 * follow the device mmbasic-epoch clock during network conformance. */
int host_time_use_mmbasic_offset = 0;

/* DATE$/TIME$ overrides for host builds: honour test mocking via the
 * MMBASIC_HOST_DATE / MMBASIC_HOST_TIME env vars, otherwise show wall-clock
 * time. When host_time_use_mmbasic_offset is set, return 0 to defer to the
 * standard mmbasic-epoch path so host network conformance matches device. */
int port_clock_format_date(char * out) {
    const char * mock = getenv("MMBASIC_HOST_DATE");
    if (mock && *mock) {
        strncpy(out, mock, 15);
        out[15] = '\0';
        return 1;
    }
    if (host_time_use_mmbasic_offset) return 0;
    time_t now = time(NULL);
    struct tm * lt = localtime(&now);
    if (!lt) return 0;
    snprintf(out, 16, "%02d-%02d-%04d", lt->tm_mday, lt->tm_mon + 1, lt->tm_year + 1900);
    return 1;
}

int port_clock_format_time(char * out) {
    const char * mock = getenv("MMBASIC_HOST_TIME");
    if (mock && *mock) {
        strncpy(out, mock, 15);
        out[15] = '\0';
        return 1;
    }
    if (host_time_use_mmbasic_offset) return 0;
    time_t now = time(NULL);
    struct tm * lt = localtime(&now);
    if (!lt) return 0;
    snprintf(out, 16, "%02d:%02d:%02d", lt->tm_hour, lt->tm_min, lt->tm_sec);
    return 1;
}

/* mSecTimer / CursorTimer are referenced as externs via Hardware_Includes.h.
 * CURSOR_OFF / CURSOR_ON come from the same chain. */

static void host_sync_msec_timer_value(uint64_t now_us) {
    mSecTimer = (int64_t)(now_us / 1000ULL);
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

void host_sleep_us(uint64_t us) {
    if (us == 0) {
        host_sync_msec_timer();
        return;
    }
    /* nanosleep is a true blocking sleep on both targets:
     *   - Native host: libc nanosleep, kernel-scheduled.
     *   - WASM under -pthread: emscripten implements this via
     *     emscripten_futex_wait (Atomics.wait on a shared-memory
     *     cell). Parks just this pthread; the worker's JS event loop
     *     stays responsive for FS round-trips on the main thread. */
    struct timespec req;
    req.tv_sec = (time_t)(us / 1000000ULL);
    req.tv_nsec = (long)((us % 1000000ULL) * 1000ULL);
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
    host_sync_msec_timer();
}
