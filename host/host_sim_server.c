/*
 * host_sim_server.c -- Mongoose HTTP + WebSocket server for --sim mode.
 *
 * Runs on a background pthread. Serves static files from `web/` and
 * broadcasts the host framebuffer as RGBA binary WS frames at ~60fps
 * to every connected client on `/ws`.
 *
 * The MMBasic interpreter writes the framebuffer on the main thread
 * without any locking. The server reads it with memcpy under no lock;
 * a torn frame survives 16ms before the next broadcast overwrites it,
 * which is invisible in practice.
 *
 * Client → server messages (JSON) are decoded minimally here but are
 * not plumbed into the runtime until Phase 2 (keyboard).
 */

#include "vendor/mongoose.h"
#include "host_sim_server.h"
#include "host_sim_audio.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern size_t host_sim_framebuffer_copy(uint32_t *dst, size_t dst_pixels);
extern void host_sim_framebuffer_dims(int *w, int *h);
extern void host_sim_push_key(int code);
extern size_t host_sim_cmd_drain(uint8_t **out_buf, size_t *out_cap);

/*
 * Per-connection state. New clients need one full-frame FRMB to bootstrap
 * the canvas; after that they just replay the shared CMDS stream the
 * server broadcasts every tick.
 */
typedef struct sim_client {
    int bootstrapped;
} sim_client;

struct sim_server {
    struct mg_mgr mgr;
    pthread_t thread;
    char listen_url[128];
    char web_root[1024];
    atomic_int running;
    uint64_t last_frame_ms;
    uint32_t *staging;
    size_t staging_capacity;
};

static struct sim_server g_server;

/*
 * Convert our local RGBA (stored as uint32_t: 0x00RRGGBB on the host side,
 * written with host_colour24 masking the top byte) into the browser's
 * canvas ImageData format: little-endian [R,G,B,A] bytes per pixel.
 */
static void rgb24_to_rgba8(const uint32_t *src, uint8_t *dst, size_t pixels) {
    for (size_t i = 0; i < pixels; ++i) {
        uint32_t c = src[i];
        dst[i * 4 + 0] = (uint8_t)((c >> 16) & 0xFF);
        dst[i * 4 + 1] = (uint8_t)((c >> 8) & 0xFF);
        dst[i * 4 + 2] = (uint8_t)(c & 0xFF);
        dst[i * 4 + 3] = 0xFF;
    }
}

/*
 * Send one full-frame FRMB snapshot to a specific client. Used once per
 * client on first broadcast after connect, so the canvas starts with the
 * current framebuffer contents. After that the client just consumes the
 * CMDS stream.
 */
static void send_bootstrap_frame(struct mg_connection *c, struct sim_server *s,
                                 int w, int h) {
    size_t pixels = (size_t)w * (size_t)h;
    if (pixels > s->staging_capacity) {
        free(s->staging);
        s->staging = calloc(pixels, sizeof(uint32_t));
        if (!s->staging) { s->staging_capacity = 0; return; }
        s->staging_capacity = pixels;
    }
    host_sim_framebuffer_copy(s->staging, pixels);

    uint8_t header[8];
    header[0] = 'F'; header[1] = 'R'; header[2] = 'M'; header[3] = 'B';
    header[4] = (uint8_t)(w & 0xFF);
    header[5] = (uint8_t)((w >> 8) & 0xFF);
    header[6] = (uint8_t)(h & 0xFF);
    header[7] = (uint8_t)((h >> 8) & 0xFF);
    size_t msg_len = sizeof(header) + pixels * 4;
    uint8_t *msg = malloc(msg_len);
    if (!msg) return;
    memcpy(msg, header, sizeof(header));
    rgb24_to_rgba8(s->staging, msg + sizeof(header), pixels);
    mg_ws_send(c, msg, msg_len, WEBSOCKET_OP_BINARY);
    free(msg);
}

/*
 * Broadcast the queued-up graphics command stream to every connected WS
 * client as one "CMDS" message. Each CMDS message is:
 *   bytes 0..3 : magic "CMDS"
 *   bytes 4..5 : canvas width  (LE u16)
 *   bytes 6..7 : canvas height (LE u16)
 *   bytes 8..  : opaque opcode stream (see host_stubs_legacy.c for format)
 *
 * Fresh clients get one FRMB snapshot first so their canvas starts with
 * whatever's already been drawn.
 */
static void broadcast_frame(struct sim_server *s) {
    int w = 0, h = 0;
    host_sim_framebuffer_dims(&w, &h);
    if (w <= 0 || h <= 0) return;

    /* Drain all queued commands once. */
    uint8_t *cmd_bytes = NULL;
    size_t cmd_cap = 0;
    size_t cmd_len = host_sim_cmd_drain(&cmd_bytes, &cmd_cap);

    /* Bootstrap any new connections with a full frame; then broadcast the
     * drained command stream (if non-empty) to everyone. */
    struct mg_connection *c;
    for (c = s->mgr.conns; c != NULL; c = c->next) {
        if (!c->is_websocket) continue;
        sim_client *cs = (sim_client *)c->data;
        _Static_assert(sizeof(sim_client) <= MG_DATA_SIZE, "sim_client too big for c->data");
        if (!cs->bootstrapped) {
            send_bootstrap_frame(c, s, w, h);
            cs->bootstrapped = 1;
        }
    }

    if (cmd_len == 0) {
        free(cmd_bytes);
        return;
    }

    uint8_t header[8];
    header[0] = 'C'; header[1] = 'M'; header[2] = 'D'; header[3] = 'S';
    header[4] = (uint8_t)(w & 0xFF);
    header[5] = (uint8_t)((w >> 8) & 0xFF);
    header[6] = (uint8_t)(h & 0xFF);
    header[7] = (uint8_t)((h >> 8) & 0xFF);
    size_t msg_len = sizeof(header) + cmd_len;
    uint8_t *msg = malloc(msg_len);
    if (!msg) { free(cmd_bytes); return; }
    memcpy(msg, header, sizeof(header));
    memcpy(msg + sizeof(header), cmd_bytes, cmd_len);
    free(cmd_bytes);

    for (c = s->mgr.conns; c != NULL; c = c->next) {
        if (!c->is_websocket) continue;
        mg_ws_send(c, msg, msg_len, WEBSOCKET_OP_BINARY);
    }
    free(msg);
}

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    struct sim_server *s = (struct sim_server *)c->fn_data;

    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        if (mg_match(hm->uri, mg_str("/ws"), NULL)) {
            mg_ws_upgrade(c, hm, NULL);
        } else {
            struct mg_http_serve_opts opts = {0};
            opts.root_dir = s->web_root;
            mg_http_serve_dir(c, hm, &opts);
        }
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
        /* Only text frames carry our JSON protocol. */
        if ((wm->flags & 0x0f) == WEBSOCKET_OP_TEXT) {
            long code = mg_json_get_long(wm->data, "$.code", -1);
            if (code >= 0 && code <= 0xff) {
                host_sim_push_key((int)code);
            }
        }
    } else if (ev == MG_EV_POLL) {
        /* No throttle: every poll iteration drains whatever commands the
         * MMBasic thread has produced and broadcasts them. With
         * mg_mgr_poll(mgr, 1) below, that's ~1ms latency end-to-end —
         * indistinguishable from immediate. broadcast_frame() is a no-op
         * when the queue is empty. */
        broadcast_frame(s);

        /* Drain audio events (JSON TEXT frames) and forward to every
         * connected WS client. Audio volume is low, so one message per
         * TEXT frame is fine — no batching needed. */
        char **audio_msgs = NULL;
        int audio_count = 0;
        host_sim_audio_drain(&audio_msgs, &audio_count);
        if (audio_count > 0) {
            for (int i = 0; i < audio_count; ++i) {
                const char *msg = audio_msgs[i];
                if (!msg) continue;
                size_t len = strlen(msg);
                for (struct mg_connection *wc = s->mgr.conns; wc; wc = wc->next) {
                    if (wc->is_websocket)
                        mg_ws_send(wc, msg, len, WEBSOCKET_OP_TEXT);
                }
            }
            host_sim_audio_free_drain(audio_msgs, audio_count);
        }
        (void)ev_data;
    }
}

static void sim_log_sink(char ch, void *param) {
    (void)ch;
    (void)param;  /* swallow all Mongoose log output — the REPL owns stdout/stderr */
}

static void *server_thread(void *arg) {
    struct sim_server *s = (struct sim_server *)arg;
    mg_log_set(MG_LL_NONE);
    mg_log_set_fn(sim_log_sink, NULL);
    mg_mgr_init(&s->mgr);
    struct mg_connection *lc = mg_http_listen(&s->mgr, s->listen_url, ev_handler, s);
    if (!lc) {
        atomic_store(&s->running, 0);
        mg_mgr_free(&s->mgr);
        return NULL;
    }
    while (atomic_load(&s->running)) {
        mg_mgr_poll(&s->mgr, 1);
    }
    mg_mgr_free(&s->mgr);
    return NULL;
}

int host_sim_server_start(const char *listen_addr, int port, const char *web_root) {
    memset(&g_server, 0, sizeof(g_server));
    snprintf(g_server.listen_url, sizeof(g_server.listen_url),
             "http://%s:%d", listen_addr ? listen_addr : "127.0.0.1", port);
    snprintf(g_server.web_root, sizeof(g_server.web_root), "%s",
             web_root ? web_root : "web");
    atomic_store(&g_server.running, 1);
    if (pthread_create(&g_server.thread, NULL, server_thread, &g_server) != 0) {
        perror("pthread_create");
        atomic_store(&g_server.running, 0);
        return -1;
    }
    return 0;
}

void host_sim_server_stop(void) {
    if (!atomic_load(&g_server.running)) return;
    atomic_store(&g_server.running, 0);
    pthread_join(g_server.thread, NULL);
    free(g_server.staging);
    g_server.staging = NULL;
    g_server.staging_capacity = 0;
}
