/*
 * host_wasm_audio.c -- Web Audio bridge for the WASM host port.
 *
 * Provides the same host_sim_audio_* symbol set that Audio.c's host body
 * calls into, but instead of queuing JSON for a WebSocket drain (the
 * native --sim path in host_sim_audio.c), each call drops straight into
 * JS via EM_ASM. The JS side (host/web/ui/audio.js) owns the actual
 * WebAudio graph.
 *
 * This file is only linked into the WASM build. The native host
 * (mmbasic_test) and --sim host (mmbasic_sim) keep using
 * host_sim_audio.c unchanged.
 *
 * The drain API (host_sim_audio_drain / _free_drain) is retained as a
 * stub because other non-sim host code paths link against the symbols;
 * the WASM build has no server thread to drain into, so these return 0.
 */

#include "host_sim_audio.h"

#include <stddef.h>

#ifdef MMBASIC_WASM

#include <emscripten.h>

/* Note on the JS side: window.picomiteAudio is created by audio.js
 * before wasm_boot() is called. If for any reason it's not there yet
 * (very early BASIC PLAY statements, headless environments without
 * audio.js loaded), the guard `if (window.picomiteAudio)` makes the
 * call a silent no-op rather than a ReferenceError. */

void host_sim_audio_tone(double left_hz, double right_hz,
                         int has_duration, long long duration_ms) {
    EM_ASM({
        if (typeof window !== 'undefined' && window.picomiteAudio) {
            window.picomiteAudio.tone($0, $1, $2 ? $3 : -1);
        }
    }, left_hz, right_hz, has_duration, (double)duration_ms);
}

void host_sim_audio_stop(void) {
    EM_ASM({
        if (typeof window !== 'undefined' && window.picomiteAudio) {
            window.picomiteAudio.stop();
        }
    });
}

void host_sim_audio_sound(int slot, const char *ch, const char *type,
                          double freq_hz, int volume) {
    EM_ASM({
        if (typeof window !== 'undefined' && window.picomiteAudio) {
            window.picomiteAudio.sound(
                $0,
                UTF8ToString($1),
                UTF8ToString($2),
                $3,
                $4);
        }
    }, slot, ch ? ch : "B", type ? type : "O", freq_hz, volume);
}

void host_sim_audio_volume(int left, int right) {
    EM_ASM({
        if (typeof window !== 'undefined' && window.picomiteAudio) {
            window.picomiteAudio.volume($0, $1);
        }
    }, left, right);
}

void host_sim_audio_pause(void) {
    EM_ASM({
        if (typeof window !== 'undefined' && window.picomiteAudio) {
            window.picomiteAudio.pause();
        }
    });
}

void host_sim_audio_resume(void) {
    EM_ASM({
        if (typeof window !== 'undefined' && window.picomiteAudio) {
            window.picomiteAudio.resume();
        }
    });
}

size_t host_sim_audio_drain(char ***out_msgs, int *out_count) {
    if (out_msgs) *out_msgs = NULL;
    if (out_count) *out_count = 0;
    return 0;
}

void host_sim_audio_free_drain(char **msgs, int count) {
    (void)msgs; (void)count;
}

#else  /* !MMBASIC_WASM — shouldn't be linked, but stub so the TU still builds. */

void host_sim_audio_tone(double l, double r, int h, long long m) {
    (void)l; (void)r; (void)h; (void)m;
}
void host_sim_audio_stop(void) {}
void host_sim_audio_sound(int s, const char *c, const char *t, double f, int v) {
    (void)s; (void)c; (void)t; (void)f; (void)v;
}
void host_sim_audio_volume(int l, int r) { (void)l; (void)r; }
void host_sim_audio_pause(void) {}
void host_sim_audio_resume(void) {}
size_t host_sim_audio_drain(char ***m, int *c) {
    if (m) *m = NULL;
    if (c) *c = 0;
    return 0;
}
void host_sim_audio_free_drain(char **m, int c) { (void)m; (void)c; }

#endif
