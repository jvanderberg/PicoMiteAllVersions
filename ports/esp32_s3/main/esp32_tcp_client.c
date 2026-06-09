#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hal/hal_filesystem.h"
#include "hal/hal_net.h"
#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "shared/net/mm_net_tcp_client_cmd.h"
#include "esp32_tcp_client.h"

#define ESP32_TCP_CLIENT_RX_CHUNK 512
#define ESP32_TLS_CA_MAX 8192

typedef struct {
    hal_net_tcp_client_t client;
    int tls;
    int stream_open;
    volatile int stream_running;
    TaskHandle_t stream_task;
    uint8_t * stream_buf;
    int stream_size;
    int64_t * stream_read;
    int64_t * stream_write;
} esp32_tcp_client_t;

static esp32_tcp_client_t s_client = {
    .client = 0,
};

static void esp32_tcp_client_stream_task(void * arg) {
    (void)arg;
    uint8_t tmp[ESP32_TCP_CLIENT_RX_CHUNK];
    while (s_client.stream_running && s_client.client) {
        size_t n = 0;
        int rc = hal_net_tcp_client_recv(s_client.client, tmp, sizeof tmp, &n, 100);
        if (rc == HAL_NET_OK && n > 0) {
            if (!s_client.stream_buf || !s_client.stream_read ||
                !s_client.stream_write || s_client.stream_size <= 1) {
                s_client.stream_running = 0;
                break;
            }
            mm_net_tcp_client_stream_append(s_client.stream_buf,
                                            s_client.stream_size,
                                            s_client.stream_read,
                                            s_client.stream_write,
                                            tmp, n);
        } else if (rc == HAL_NET_TIMEOUT || rc == HAL_NET_WOULD_BLOCK ||
                   (rc == HAL_NET_OK && n == 0)) {
            vTaskDelay(pdMS_TO_TICKS(1));
        } else {
            break;
        }
    }
    s_client.stream_running = 0;
    s_client.stream_task = NULL;
    vTaskDelete(NULL);
}

static void esp32_tcp_client_stop_stream(void) {
    if (!s_client.stream_task) {
        s_client.stream_running = 0;
        return;
    }
    s_client.stream_running = 0;
    for (int i = 0; i < 100 && s_client.stream_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void esp32_tcp_client_close(void) {
    esp32_tcp_client_stop_stream();
    if (s_client.client) {
        hal_net_tcp_client_close(s_client.client);
    }
    memset(&s_client, 0, sizeof s_client);
}

static void esp32_tcp_client_open_cmd(unsigned char * arg, int stream_open,
                                      int force_tls) {
    mm_net_tcp_client_open_args_t parsed;
    mm_net_tcp_client_parse_open(arg, &parsed);
    if (force_tls) parsed.tls = 1;
    if (parsed.tls &&
        !(hal_net_capabilities() & HAL_NET_CAP_TCP_CLIENT_TLS))
        error("TLS not supported on this port");

    esp32_tcp_client_close();
    if (hal_net_tcp_client_open(parsed.host, (uint16_t)parsed.port,
                                (uint32_t)parsed.timeout_ms, parsed.tls,
                                &s_client.client) != HAL_NET_OK)
        error("No response from client");

    s_client.tls = parsed.tls;
    s_client.stream_open = stream_open;
    MMPrintString("Connected\r\n");
}

static void esp32_tcp_client_request_cmd(unsigned char * arg) {
    if (!s_client.client) error("No connection");
    if (s_client.stream_running) error("Connection busy");

    mm_net_tcp_client_request_args_t parsed;
    mm_net_tcp_client_parse_request(arg, &parsed);

    if (parsed.request_len &&
        hal_net_tcp_client_send(s_client.client, parsed.request,
                                parsed.request_len,
                                (uint32_t)parsed.timeout_ms) != HAL_NET_OK)
        error("write failed");

    int total = 0;
    uint32_t wait_ms = (uint32_t)parsed.timeout_ms;
    while (total < parsed.payload_capacity) {
        size_t got = 0;
        int rc = hal_net_tcp_client_recv(
            s_client.client, parsed.buffer + total,
            (size_t)(parsed.payload_capacity - total), &got, wait_ms);
        if (rc == HAL_NET_OK && got > 0) {
            total += (int)got;
            wait_ms = 200;
            routinechecks();
            continue;
        }
        if (total == 0 && rc != HAL_NET_OK) error("No response from server");
        break;
    }
    parsed.dest[0] = total;
}

static void esp32_tcp_client_stream_cmd(unsigned char * arg) {
    if (!s_client.client) error("No connection");

    mm_net_tcp_client_stream_args_t parsed;
    mm_net_tcp_client_parse_stream(arg, &parsed);

    esp32_tcp_client_stop_stream();

    s_client.stream_buf = parsed.buffer;
    s_client.stream_size = parsed.payload_capacity;
    s_client.stream_read = parsed.read_pos;
    s_client.stream_write = parsed.write_pos;
    if (*parsed.read_pos < 0 || *parsed.read_pos >= parsed.payload_capacity)
        *parsed.read_pos = 0;
    if (*parsed.write_pos < 0 || *parsed.write_pos >= parsed.payload_capacity)
        *parsed.write_pos = *parsed.read_pos;

    if (parsed.request_len &&
        hal_net_tcp_client_send(s_client.client, parsed.request,
                                parsed.request_len, 5000) != HAL_NET_OK)
        error("write failed");

    s_client.stream_running = 1;
    if (xTaskCreate(esp32_tcp_client_stream_task, "mmbasic_tcp_client",
                    4096, NULL, 5, &s_client.stream_task) != pdPASS) {
        s_client.stream_running = 0;
        error("Failed to create TCP client");
    }
}

static void esp32_tls_cert_cmd(unsigned char * arg) {
    char * fname = (char *)getCstring(arg);
    if (!*fname) {
        hal_net_tls_set_ca(NULL, 0);
        return;
    }

    struct hal_stat st;
    if (hal_fs_stat(fname, &st) < 0 || !(st.mode & HAL_FS_S_IFREG))
        error("Cannot find file");
    if (st.size <= 0 || st.size > ESP32_TLS_CA_MAX)
        error("Invalid CA certificate");

    hal_fs_fd_t fd;
    if (hal_fs_open(fname, HAL_FS_O_RDONLY, &fd) < 0)
        error("Cannot find file");
    char * pem = (char *)GetTempMemory((int)st.size + 1);
    off_t total = 0;
    while (total < st.size) {
        ssize_t n = hal_fs_read(fd, pem + total, (size_t)(st.size - total));
        if (n <= 0) break;
        total += n;
    }
    hal_fs_close(fd);
    if (total != st.size) error("Cannot read file");
    pem[total] = 0;
    if (!strstr(pem, "-----BEGIN")) error("Invalid CA certificate");
    if (hal_net_tls_set_ca(pem, (size_t)total) != HAL_NET_OK)
        error("TLS not supported on this port");
}

int esp32_tcp_client_cmd(unsigned char * line) {
    unsigned char * tp;
    if ((tp = checkstring(line, (unsigned char *)"TLS CERT"))) {
        esp32_tls_cert_cmd(tp);
        return 1;
    }
    if ((tp = checkstring(line, (unsigned char *)"OPEN TCP CLIENT"))) {
        esp32_tcp_client_open_cmd(tp, 0, 0);
        return 1;
    }
    if ((tp = checkstring(line, (unsigned char *)"OPEN TCP STREAM"))) {
        esp32_tcp_client_open_cmd(tp, 1, 0);
        return 1;
    }
    if ((tp = checkstring(line, (unsigned char *)"OPEN TLS CLIENT"))) {
        esp32_tcp_client_open_cmd(tp, 0, 1);
        return 1;
    }
    if ((tp = checkstring(line, (unsigned char *)"OPEN TLS STREAM"))) {
        esp32_tcp_client_open_cmd(tp, 1, 1);
        return 1;
    }
    if ((tp = checkstring(line, (unsigned char *)"TCP CLIENT REQUEST"))) {
        esp32_tcp_client_request_cmd(tp);
        return 1;
    }
    if ((tp = checkstring(line, (unsigned char *)"TLS CLIENT REQUEST"))) {
        if (s_client.client && !s_client.tls) error("Not a TLS connection");
        esp32_tcp_client_request_cmd(tp);
        return 1;
    }
    if ((tp = checkstring(line, (unsigned char *)"TCP CLIENT STREAM"))) {
        esp32_tcp_client_stream_cmd(tp);
        return 1;
    }
    if ((tp = checkstring(line, (unsigned char *)"TLS CLIENT STREAM"))) {
        if (s_client.client && !s_client.tls) error("Not a TLS connection");
        esp32_tcp_client_stream_cmd(tp);
        return 1;
    }
    if ((tp = checkstring(line, (unsigned char *)"CLOSE TCP CLIENT")) ||
        (tp = checkstring(line, (unsigned char *)"CLOSE TLS CLIENT"))) {
        if (*tp) error("Syntax");
        if (!s_client.client) error("No connection");
        esp32_tcp_client_close();
        return 1;
    }
    return 0;
}
