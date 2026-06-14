/*
 * ports/esp32_s3/main/hal_net_esp32.c - ESP-IDF socket network HAL.
 *
 * This is the transport-facing slice of the ESP32 network backend. The
 * BASIC-visible WEB command surface still lives in esp32_wifi.c during the
 * migration; shared network code can use this file through hal_net.h.
 */

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

#include "hal/hal_net.h"
#include "vga_lcdcam_s3.h" /* vga_lcdcam_s3_scanout_reserved() */

#define ESP32_NET_MAX_TCP_SERVERS 4
#define ESP32_NET_MAX_TCP_CONNS 12
#define ESP32_NET_MAX_TCP_CLIENTS 8
#define ESP32_NET_MAX_UDP_SOCKS 8
#define ESP32_NET_MAX_MQTT_CLIENTS 4
#define ESP32_NET_MQTT_TOPIC_MAX 256
#define ESP32_NET_MQTT_PAYLOAD_MAX 256
#define ESP32_NET_WIFI_MAX_RETRIES 5

extern volatile int WIFIconnected;
extern int startupcomplete;

typedef struct {
    int fd;          /* plain-TCP socket, -1 when the slot is unused */
    esp_tls_t * tls; /* TLS session, NULL for plain connections */
} esp32_net_tcp_client_slot_t;

typedef struct {
    esp_mqtt_client_handle_t client;
    hal_net_tcp_client_t plain_tcp;
    uint16_t packet_id;
    volatile int connected;
    volatile int subscribed;
    volatile int unsubscribed;
    volatile int pending;
    char host[256];
    char user[256];
    char pass[256];
    char client_id[64];
    char topic[ESP32_NET_MQTT_TOPIC_MAX];
    uint8_t payload[ESP32_NET_MQTT_PAYLOAD_MAX];
    size_t payload_len;
} esp32_net_mqtt_slot_t;

/* Custom CA installed by hal_net_tls_set_ca; NUL-terminated PEM. When
 * NULL, TLS connections verify against the ESP-IDF certificate bundle. */
static char * tls_ca_pem;
static size_t tls_ca_len;

static int tcp_servers[ESP32_NET_MAX_TCP_SERVERS];
static int tcp_conns[ESP32_NET_MAX_TCP_CONNS];
static esp32_net_tcp_client_slot_t tcp_clients[ESP32_NET_MAX_TCP_CLIENTS];
static int udp_socks[ESP32_NET_MAX_UDP_SOCKS];
static esp32_net_mqtt_slot_t mqtt_clients[ESP32_NET_MAX_MQTT_CLIENTS];
static int tables_ready;

static int wifi_ready;
static int wifi_started;
static int wifi_retry_count;
static int wifi_last_status;
static esp_netif_t * wifi_sta_netif;
static char wifi_ssid[64];
static char wifi_pass[64];
static char wifi_host[32];
static char wifi_ip[16];
static char wifi_mask[16];
static char wifi_gw[16];

static void esp32_net_init_tables(void) {
    if (tables_ready) return;
    for (size_t i = 0; i < ESP32_NET_MAX_TCP_SERVERS; ++i) tcp_servers[i] = -1;
    for (size_t i = 0; i < ESP32_NET_MAX_TCP_CONNS; ++i) tcp_conns[i] = -1;
    for (size_t i = 0; i < ESP32_NET_MAX_TCP_CLIENTS; ++i) {
        tcp_clients[i].fd = -1;
        tcp_clients[i].tls = NULL;
    }
    for (size_t i = 0; i < ESP32_NET_MAX_UDP_SOCKS; ++i) udp_socks[i] = -1;
    memset(mqtt_clients, 0, sizeof mqtt_clients);
    tables_ready = 1;
}

static int esp32_net_set_nonblock(int fd, int enabled) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return HAL_NET_ERR;
    if (enabled)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags) == 0 ? HAL_NET_OK : HAL_NET_ERR;
}

static uint16_t esp32_net_alloc(int * slots, size_t count, int fd) {
    esp32_net_init_tables();
    for (size_t i = 0; i < count; ++i) {
        if (slots[i] < 0) {
            slots[i] = fd;
            return (uint16_t)(i + 1);
        }
    }
    return 0;
}

static int * esp32_net_slot(int * slots, size_t count, uint16_t handle) {
    if (handle == 0 || handle > count) return NULL;
    if (slots[handle - 1] < 0) return NULL;
    return &slots[handle - 1];
}

static int esp32_net_close_slot(int * slots, size_t count, uint16_t handle) {
    int * slot = esp32_net_slot(slots, count, handle);
    if (!slot) return HAL_NET_ERR;
    shutdown(*slot, SHUT_RDWR);
    close(*slot);
    *slot = -1;
    return HAL_NET_OK;
}

static int esp32_net_wait_fd(int fd, int for_write, uint32_t timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);

    struct timeval tv;
    struct timeval * tvp = NULL;
    if (timeout_ms) {
        tv.tv_sec = (time_t)(timeout_ms / 1000);
        tv.tv_usec = (suseconds_t)((timeout_ms % 1000) * 1000);
        tvp = &tv;
    }

    int rc = select(fd + 1, for_write ? NULL : &set, for_write ? &set : NULL,
                    NULL, tvp);
    if (rc > 0) return HAL_NET_OK;
    if (rc == 0) return HAL_NET_TIMEOUT;
    if (errno == EINTR) return HAL_NET_TIMEOUT;
    return HAL_NET_ERR;
}

static int esp32_net_send_all(int fd, const void * buf, size_t len,
                              uint32_t timeout_ms) {
    const uint8_t * p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        int wait = esp32_net_wait_fd(fd, 1, timeout_ms);
        if (wait != HAL_NET_OK) return wait;
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        return HAL_NET_ERR;
    }
    return HAL_NET_OK;
}

static esp32_net_tcp_client_slot_t * esp32_net_tcp_client_slot(
    hal_net_tcp_client_t handle) {
    if (handle == 0 || handle > ESP32_NET_MAX_TCP_CLIENTS) return NULL;
    esp32_net_tcp_client_slot_t * slot = &tcp_clients[handle - 1];
    if (slot->fd < 0 && !slot->tls) return NULL;
    return slot;
}

static hal_net_tcp_client_t esp32_net_tcp_client_alloc(int fd,
                                                       esp_tls_t * tls) {
    esp32_net_init_tables();
    for (size_t i = 0; i < ESP32_NET_MAX_TCP_CLIENTS; ++i) {
        if (tcp_clients[i].fd < 0 && !tcp_clients[i].tls) {
            tcp_clients[i].fd = fd;
            tcp_clients[i].tls = tls;
            return (hal_net_tcp_client_t)(i + 1);
        }
    }
    return 0;
}

static int esp32_net_tls_sockfd(esp_tls_t * tls) {
    int fd = -1;
    if (esp_tls_get_conn_sockfd(tls, &fd) != ESP_OK) return -1;
    return fd;
}

static int esp32_net_tls_send_all(esp_tls_t * tls, const void * buf,
                                  size_t len, uint32_t timeout_ms) {
    const uint8_t * p = (const uint8_t *)buf;
    size_t sent = 0;
    int fd = esp32_net_tls_sockfd(tls);
    if (fd < 0) return HAL_NET_ERR;
    while (sent < len) {
        ssize_t n = esp_tls_conn_write(tls, p + sent, len - sent);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            int wait = esp32_net_wait_fd(
                fd, n == ESP_TLS_ERR_SSL_WANT_WRITE, timeout_ms);
            if (wait != HAL_NET_OK) return wait;
            continue;
        }
        return HAL_NET_ERR;
    }
    return HAL_NET_OK;
}

static int esp32_net_tls_recv(esp_tls_t * tls, void * buf, size_t cap,
                              size_t * len, uint32_t timeout_ms) {
    /* TLS records may already be buffered inside mbedTLS, in which case the
     * socket never becomes readable — drain the session buffer first. */
    if (esp_tls_get_bytes_avail(tls) <= 0) {
        int fd = esp32_net_tls_sockfd(tls);
        if (fd < 0) return HAL_NET_ERR;
        int wait = esp32_net_wait_fd(fd, 0, timeout_ms);
        if (wait != HAL_NET_OK) return wait;
    }
    ssize_t n = esp_tls_conn_read(tls, buf, cap);
    if (n > 0) {
        if (len) *len = (size_t)n;
        return HAL_NET_OK;
    }
    if (n == 0) return HAL_NET_OK;
    if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE)
        return HAL_NET_WOULD_BLOCK;
    return HAL_NET_ERR;
}

static int esp32_net_resolve_addr(const char * host, uint16_t port, int socktype,
                                  struct addrinfo ** out) {
    char portbuf[16];
    snprintf(portbuf, sizeof portbuf, "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socktype;
    return getaddrinfo(host, portbuf, &hints, out) == 0 ? HAL_NET_OK : HAL_NET_ERR;
}

static void esp32_net_wifi_event_handler(void * arg, esp_event_base_t event_base,
                                         int32_t event_id, void * event_data) {
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        WIFIconnected = 0;
        if (wifi_retry_count < ESP32_NET_WIFI_MAX_RETRIES) {
            wifi_retry_count++;
            esp_wifi_connect();
        } else {
            wifi_last_status = -1;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        (void)event_data;
        wifi_retry_count = 0;
        wifi_last_status = 1;
        WIFIconnected = 1;
    }
}

void esp32_mbedtls_mem_install(void);

static int esp32_net_wifi_ensure_ready(void) {
    if (wifi_ready) return HAL_NET_OK;

    esp32_mbedtls_mem_install();

    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    esp_log_level_set("pp", ESP_LOG_WARN);
    esp_log_level_set("net80211", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) return HAL_NET_ERR;

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return HAL_NET_ERR;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return HAL_NET_ERR;

    /* A previous attempt may have created the netif and then failed in
     * esp_wifi_init (e.g. out of internal RAM); the default STA netif key
     * is unique, so never create it twice. */
    if (!wifi_sta_netif) wifi_sta_netif = esp_netif_create_default_wifi_sta();
    if (!wifi_sta_netif) return HAL_NET_ERR;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return HAL_NET_ERR;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    /* The CYD audio amp shares a noisy board power domain. Avoid modem-sleep
     * wake bursts coupling into the always-on speaker path as low-rate clicks. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power(8); /* 2 dBm; reduce RF current spikes near the amp. */

    if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            esp32_net_wifi_event_handler, NULL,
                                            NULL) != ESP_OK) return HAL_NET_ERR;
    if (esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            esp32_net_wifi_event_handler, NULL,
                                            NULL) != ESP_OK) return HAL_NET_ERR;

    wifi_ready = 1;
    startupcomplete = 1;
    return HAL_NET_OK;
}

static void esp32_net_wifi_apply_ip_config(void) {
    if (!wifi_sta_netif) return;

    if (*wifi_host) esp_netif_set_hostname(wifi_sta_netif, wifi_host);

    if (*wifi_ip) {
        esp_netif_ip_info_t info;
        memset(&info, 0, sizeof info);
        esp_netif_str_to_ip4(wifi_ip, &info.ip);
        esp_netif_str_to_ip4(wifi_mask, &info.netmask);
        esp_netif_str_to_ip4(wifi_gw, &info.gw);
        esp_netif_dhcpc_stop(wifi_sta_netif);
        esp_netif_set_ip_info(wifi_sta_netif, &info);
    } else {
        esp_netif_dhcpc_start(wifi_sta_netif);
    }
}

static void esp32_net_copy_string(char * dst, size_t dst_len, const char * src) {
    if (!dst || dst_len == 0) return;
    if (!src) src = "";
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = 0;
}

static hal_net_mqtt_client_t esp32_net_alloc_mqtt_slot(void) {
    esp32_net_init_tables();
    for (size_t i = 0; i < ESP32_NET_MAX_MQTT_CLIENTS; ++i) {
        if (!mqtt_clients[i].client && !mqtt_clients[i].plain_tcp) {
            memset(&mqtt_clients[i], 0, sizeof mqtt_clients[i]);
            return (hal_net_mqtt_client_t)(i + 1);
        }
    }
    return 0;
}

static esp32_net_mqtt_slot_t * esp32_net_mqtt_slot(hal_net_mqtt_client_t handle) {
    if (handle == 0 || handle > ESP32_NET_MAX_MQTT_CLIENTS) return NULL;
    if (!mqtt_clients[handle - 1].client && !mqtt_clients[handle - 1].plain_tcp)
        return NULL;
    return &mqtt_clients[handle - 1];
}

static void esp32_net_mqtt_make_client_id(char * out, size_t out_len) {
    uint8_t mac[6] = {0};
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(out, out_len, "WebMite%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(out, out_len, "WebMiteESP32");
    }
}

static int esp32_net_wait_flag(volatile int * flag, uint32_t timeout_ms) {
    uint32_t waited = 0;
    do {
        if (*flag) return HAL_NET_OK;
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    } while (waited < timeout_ms);
    return *flag ? HAL_NET_OK : HAL_NET_TIMEOUT;
}

static void esp32_net_mqtt_event_handler(void * handler_args,
                                         esp_event_base_t base,
                                         int32_t event_id,
                                         void * event_data) {
    (void)base;
    esp32_net_mqtt_slot_t * slot = (esp32_net_mqtt_slot_t *)handler_args;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    if (!slot || !event) return;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        slot->connected = 1;
        break;
    case MQTT_EVENT_DISCONNECTED:
        slot->connected = 0;
        break;
    case MQTT_EVENT_SUBSCRIBED:
        slot->subscribed = 1;
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        slot->unsubscribed = 1;
        break;
    case MQTT_EVENT_DATA: {
        int tlen = event->topic_len;
        int dlen = event->data_len;
        if (tlen < 0) tlen = 0;
        if (dlen < 0) dlen = 0;
        if (tlen >= ESP32_NET_MQTT_TOPIC_MAX)
            tlen = ESP32_NET_MQTT_TOPIC_MAX - 1;
        if (dlen > ESP32_NET_MQTT_PAYLOAD_MAX)
            dlen = ESP32_NET_MQTT_PAYLOAD_MAX;
        memcpy(slot->topic, event->topic, (size_t)tlen);
        slot->topic[tlen] = 0;
        memcpy(slot->payload, event->data, (size_t)dlen);
        slot->payload_len = (size_t)dlen;
        slot->pending = 1;
        break;
    }
    default:
        break;
    }
}

uint32_t hal_net_capabilities(void) {
    return HAL_NET_CAP_TCP_SERVER |
           HAL_NET_CAP_TCP_CLIENT |
           HAL_NET_CAP_TCP_STREAM |
           HAL_NET_CAP_UDP_SERVER |
           HAL_NET_CAP_UDP_SEND |
           HAL_NET_CAP_WIFI_SCAN |
           HAL_NET_CAP_WIFI_CONNECT |
           HAL_NET_CAP_MQTT_PLAIN |
           HAL_NET_CAP_MQTT_TLS |
           HAL_NET_CAP_TCP_CLIENT_TLS;
}

int hal_net_init(void) {
    esp32_net_init_tables();
    return HAL_NET_OK;
}

int hal_net_tls_set_ca(const void * pem, size_t len) {
    free(tls_ca_pem);
    tls_ca_pem = NULL;
    tls_ca_len = 0;
    if (!pem || !len) return HAL_NET_OK;
    char * copy = malloc(len + 1);
    if (!copy) return HAL_NET_ERR;
    memcpy(copy, pem, len);
    copy[len] = 0;
    tls_ca_pem = copy;
    tls_ca_len = len;
    return HAL_NET_OK;
}

void hal_net_poll(void) {
}

int hal_net_wifi_set_credentials(const char * ssid, const char * pass,
                                 const char * host, const char * ip,
                                 const char * mask, const char * gw) {
    esp32_net_copy_string(wifi_ssid, sizeof wifi_ssid, ssid);
    esp32_net_copy_string(wifi_pass, sizeof wifi_pass, pass);
    esp32_net_copy_string(wifi_host, sizeof wifi_host, host);
    esp32_net_copy_string(wifi_ip, sizeof wifi_ip, ip);
    esp32_net_copy_string(wifi_mask, sizeof wifi_mask, mask);
    esp32_net_copy_string(wifi_gw, sizeof wifi_gw, gw);
    return HAL_NET_OK;
}

int hal_net_wifi_connect(uint32_t timeout_ms) {
    if (esp32_net_wifi_ensure_ready() != HAL_NET_OK) {
        WIFIconnected = 0;
        wifi_last_status = -1;
        return HAL_NET_ERR;
    }

    if (!*wifi_ssid) {
        if (!wifi_started) {
            esp_wifi_set_mode(WIFI_MODE_STA);
            if (esp_wifi_start() == ESP_OK) wifi_started = 1;
        }
        return HAL_NET_OK;
    }

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof wifi_config);
    strncpy((char *)wifi_config.sta.ssid, wifi_ssid,
            sizeof wifi_config.sta.ssid - 1);
    strncpy((char *)wifi_config.sta.password, wifi_pass,
            sizeof wifi_config.sta.password - 1);
    wifi_config.sta.threshold.authmode = *wifi_pass ? WIFI_AUTH_WPA2_PSK
                                                    : WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    wifi_retry_count = 0;
    wifi_last_status = 0;
    WIFIconnected = 0;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp32_net_wifi_apply_ip_config();
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    if (!wifi_started) {
        if (esp_wifi_start() == ESP_OK) wifi_started = 1;
    } else {
        esp_wifi_disconnect();
        esp_wifi_connect();
    }

    uint32_t waited = 0;
    uint32_t limit = timeout_ms ? timeout_ms : 30000;
    char ipbuf[32];
    while (waited < limit) {
        if (WIFIconnected && hal_net_ip_address(ipbuf, sizeof ipbuf) == HAL_NET_OK)
            return HAL_NET_OK;
        if (wifi_last_status < 0) break;
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    WIFIconnected = 0;
    wifi_last_status = -1;
    return HAL_NET_TIMEOUT;
}

void esp32_net_wifi_pause_for_audio(void) {
    if (!wifi_ready) return;
    wifi_retry_count = ESP32_NET_WIFI_MAX_RETRIES;
    if (wifi_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                 esp32_net_wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                 esp32_net_wifi_event_handler);
    esp_wifi_deinit();
    wifi_ready = 0;
    wifi_started = 0;
    WIFIconnected = 0;
    wifi_last_status = 0;
}

int hal_net_wifi_status(void) {
    return WIFIconnected ? 1 : wifi_last_status;
}

int hal_net_tcpip_status(void) {
    return WIFIconnected ? 1 : wifi_last_status;
}

int hal_net_link_up(void) {
    return WIFIconnected != 0;
}

int hal_net_ip_address(char * out, size_t out_len) {
    if (!out || out_len == 0) return HAL_NET_ERR;
    out[0] = 0;

    esp_netif_t * netif = wifi_sta_netif;
    if (!netif) netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return HAL_NET_ERR;

    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0)
        return HAL_NET_ERR;
    esp_ip4addr_ntoa(&info.ip, out, (int)out_len);
    return HAL_NET_OK;
}

int hal_net_wifi_scan(char * out, size_t out_len, size_t * written,
                      int print_to_console) {
    (void)print_to_console;
    if (out && out_len) out[0] = 0;
    if (written) *written = 0;

    if (!out || out_len == 0) return HAL_NET_ERR;
    if (esp32_net_wifi_ensure_ready() != HAL_NET_OK) return HAL_NET_ERR;

    esp_wifi_set_mode(WIFI_MODE_STA);
    if (!wifi_started) {
        if (esp_wifi_start() == ESP_OK) wifi_started = 1;
    }

    wifi_scan_config_t scan_config;
    memset(&scan_config, 0, sizeof scan_config);
    if (esp_wifi_scan_start(&scan_config, true) != ESP_OK) return HAL_NET_ERR;

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    wifi_ap_record_t * records = NULL;
    if (ap_count) {
        records = malloc(sizeof(*records) * ap_count);
        if (!records) return HAL_NET_ERR;
        if (esp_wifi_scan_get_ap_records(&ap_count, records) != ESP_OK) {
            free(records);
            return HAL_NET_ERR;
        }
    }

    size_t used = 0;
    for (uint16_t i = 0; i < ap_count; i++) {
        char line[160];
        int n = snprintf(line, sizeof line,
                         "ssid: %-32s rssi: %4d chan: %3d mac: %02x:%02x:%02x:%02x:%02x:%02x sec: %u\r\n",
                         (char *)records[i].ssid, records[i].rssi,
                         records[i].primary,
                         records[i].bssid[0], records[i].bssid[1],
                         records[i].bssid[2], records[i].bssid[3],
                         records[i].bssid[4], records[i].bssid[5],
                         records[i].authmode);
        if (n < 0) {
            free(records);
            return HAL_NET_ERR;
        }
        if (used + (size_t)n + 1 > out_len) {
            free(records);
            return HAL_NET_ERR;
        }
        memcpy(out + used, line, (size_t)n);
        used += (size_t)n;
        out[used] = 0;
    }
    free(records);
    if (written) *written = used;
    return HAL_NET_OK;
}

int hal_net_tcp_server_open(uint16_t port, int backlog,
                            hal_net_tcp_server_t * out) {
    if (!out) return HAL_NET_ERR;
    *out = 0;
    esp32_net_init_tables();

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return HAL_NET_ERR;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(fd, backlog > 0 ? backlog : 1) != 0 ||
        esp32_net_set_nonblock(fd, 1) != HAL_NET_OK) {
        close(fd);
        return HAL_NET_ERR;
    }

    uint16_t handle = esp32_net_alloc(tcp_servers, ESP32_NET_MAX_TCP_SERVERS, fd);
    if (!handle) {
        close(fd);
        return HAL_NET_ERR;
    }
    *out = handle;
    return HAL_NET_OK;
}

int hal_net_tcp_server_close(hal_net_tcp_server_t server) {
    return esp32_net_close_slot(tcp_servers, ESP32_NET_MAX_TCP_SERVERS, server);
}

int hal_net_tcp_accept_conn(hal_net_tcp_server_t server,
                            hal_net_tcp_conn_t * conn) {
    if (conn) *conn = 0;
    int * slot = esp32_net_slot(tcp_servers, ESP32_NET_MAX_TCP_SERVERS, server);
    if (!slot) return HAL_NET_ERR;

    int fd = accept(*slot, NULL, NULL);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return HAL_NET_WOULD_BLOCK;
        return HAL_NET_ERR;
    }

    if (esp32_net_set_nonblock(fd, 1) != HAL_NET_OK) {
        close(fd);
        return HAL_NET_ERR;
    }

    uint16_t handle = esp32_net_alloc(tcp_conns, ESP32_NET_MAX_TCP_CONNS, fd);
    if (!handle) {
        close(fd);
        return HAL_NET_ERR;
    }
    if (conn) *conn = handle;
    return HAL_NET_OK;
}

int hal_net_tcp_accept_event(hal_net_tcp_server_t server,
                             hal_net_tcp_conn_t * conn,
                             uint8_t * buf, size_t cap, size_t * len) {
    if (conn) *conn = 0;
    if (len) *len = 0;
    hal_net_tcp_conn_t handle = 0;
    int rc = hal_net_tcp_accept_conn(server, &handle);
    if (rc != HAL_NET_OK) return rc;
    if (conn) *conn = handle;

    if (buf && cap) {
        int * conn_slot = esp32_net_slot(tcp_conns, ESP32_NET_MAX_TCP_CONNS,
                                         handle);
        if (!conn_slot) return HAL_NET_ERR;
        int fd = *conn_slot;
        int wait = esp32_net_wait_fd(fd, 0, 250);
        if (wait == HAL_NET_OK) {
            ssize_t n = recv(fd, buf, cap, 0);
            if (n > 0 && len)
                *len = (size_t)n;
            else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                esp32_net_close_slot(tcp_conns, ESP32_NET_MAX_TCP_CONNS,
                                     handle);
                return HAL_NET_ERR;
            }
        } else if (wait != HAL_NET_TIMEOUT) {
            esp32_net_close_slot(tcp_conns, ESP32_NET_MAX_TCP_CONNS, handle);
            return wait;
        }
    }
    return HAL_NET_OK;
}

int hal_net_tcp_conn_recv(hal_net_tcp_conn_t conn, void * buf, size_t cap,
                          size_t * len) {
    if (len) *len = 0;
    int * slot = esp32_net_slot(tcp_conns, ESP32_NET_MAX_TCP_CONNS, conn);
    if (!slot || !buf || cap == 0) return HAL_NET_ERR;
    ssize_t n = recv(*slot, buf, cap, 0);
    if (n > 0) {
        if (len) *len = (size_t)n;
        return HAL_NET_OK;
    }
    if (n == 0) return HAL_NET_ERR;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return HAL_NET_WOULD_BLOCK;
    return HAL_NET_ERR;
}

int hal_net_tcp_conn_send_some(hal_net_tcp_conn_t conn, const void * buf,
                               size_t cap, size_t * sent) {
    if (sent) *sent = 0;
    int * slot = esp32_net_slot(tcp_conns, ESP32_NET_MAX_TCP_CONNS, conn);
    if (!slot || (!buf && cap)) return HAL_NET_ERR;
    if (cap == 0) return HAL_NET_OK;
    ssize_t n = send(*slot, buf, cap, 0);
    if (n > 0) {
        if (sent) *sent = (size_t)n;
        return HAL_NET_OK;
    }
    if (n == 0) return HAL_NET_WOULD_BLOCK;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return HAL_NET_WOULD_BLOCK;
    return HAL_NET_ERR;
}

int hal_net_tcp_conn_send(hal_net_tcp_conn_t conn, const void * buf, size_t len,
                          uint32_t timeout_ms) {
    int * slot = esp32_net_slot(tcp_conns, ESP32_NET_MAX_TCP_CONNS, conn);
    if (!slot) return HAL_NET_ERR;
    return esp32_net_send_all(*slot, buf, len, timeout_ms);
}

int hal_net_tcp_conn_close(hal_net_tcp_conn_t conn) {
    return esp32_net_close_slot(tcp_conns, ESP32_NET_MAX_TCP_CONNS, conn);
}

int hal_net_tcp_client_open(const char * host, uint16_t port,
                            uint32_t timeout_ms, int tls,
                            hal_net_tcp_client_t * out) {
    if (!host || !out) return HAL_NET_ERR;
    *out = 0;
    esp32_net_init_tables();

    if (tls) {
        esp_tls_cfg_t cfg = {
            .timeout_ms = (int)(timeout_ms ? timeout_ms : 5000),
        };
        if (tls_ca_pem) {
            /* mbedTLS PEM parsing requires the terminating NUL in the
             * buffer length. */
            cfg.cacert_buf = (const unsigned char *)tls_ca_pem;
            cfg.cacert_bytes = (unsigned int)(tls_ca_len + 1);
        } else {
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
        }
        esp_tls_t * session = esp_tls_init();
        if (!session) return HAL_NET_ERR;
        if (esp_tls_conn_new_sync(host, (int)strlen(host), port, &cfg,
                                  session) != 1) {
            esp_tls_conn_destroy(session);
            return HAL_NET_TIMEOUT;
        }
        /* Non-blocking after the handshake so reads gated by select()
         * surface WANT_READ instead of stalling on partial records. */
        int fd = esp32_net_tls_sockfd(session);
        if (fd >= 0) esp32_net_set_nonblock(fd, 1);
        hal_net_tcp_client_t handle = esp32_net_tcp_client_alloc(-1, session);
        if (!handle) {
            esp_tls_conn_destroy(session);
            return HAL_NET_ERR;
        }
        *out = handle;
        return HAL_NET_OK;
    }

    struct addrinfo * ai = NULL;
    if (esp32_net_resolve_addr(host, port, SOCK_STREAM, &ai) != HAL_NET_OK)
        return HAL_NET_ERR;

    int fd = -1;
    int ok = 0;
    for (struct addrinfo * rp = ai; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        esp32_net_set_nonblock(fd, 1);
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            ok = 1;
        } else if (errno == EINPROGRESS) {
            int wait = esp32_net_wait_fd(fd, 1, timeout_ms);
            if (wait == HAL_NET_OK) {
                int so_error = 0;
                socklen_t so_len = sizeof so_error;
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) == 0 &&
                    so_error == 0) {
                    ok = 1;
                }
            }
        }
        if (ok) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);
    if (!ok || fd < 0) return HAL_NET_TIMEOUT;

    esp32_net_set_nonblock(fd, 0);
    hal_net_tcp_client_t handle = esp32_net_tcp_client_alloc(fd, NULL);
    if (!handle) {
        close(fd);
        return HAL_NET_ERR;
    }
    *out = handle;
    return HAL_NET_OK;
}

int hal_net_tcp_client_send(hal_net_tcp_client_t client, const void * buf,
                            size_t len, uint32_t timeout_ms) {
    esp32_net_tcp_client_slot_t * slot = esp32_net_tcp_client_slot(client);
    if (!slot) return HAL_NET_ERR;
    if (slot->tls) return esp32_net_tls_send_all(slot->tls, buf, len, timeout_ms);
    return esp32_net_send_all(slot->fd, buf, len, timeout_ms);
}

int hal_net_tcp_client_recv(hal_net_tcp_client_t client, void * buf,
                            size_t cap, size_t * len, uint32_t timeout_ms) {
    if (len) *len = 0;
    esp32_net_tcp_client_slot_t * slot = esp32_net_tcp_client_slot(client);
    if (!slot || !buf || cap == 0) return HAL_NET_ERR;
    if (slot->tls) return esp32_net_tls_recv(slot->tls, buf, cap, len, timeout_ms);
    int wait = esp32_net_wait_fd(slot->fd, 0, timeout_ms);
    if (wait != HAL_NET_OK) return wait;
    ssize_t n = recv(slot->fd, buf, cap, 0);
    if (n > 0) {
        if (len) *len = (size_t)n;
        return HAL_NET_OK;
    }
    if (n == 0) return HAL_NET_OK;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return HAL_NET_WOULD_BLOCK;
    return HAL_NET_ERR;
}

int hal_net_tcp_client_close(hal_net_tcp_client_t client) {
    esp32_net_tcp_client_slot_t * slot = esp32_net_tcp_client_slot(client);
    if (!slot) return HAL_NET_ERR;
    if (slot->tls) {
        esp_tls_conn_destroy(slot->tls);
    } else {
        shutdown(slot->fd, SHUT_RDWR);
        close(slot->fd);
    }
    slot->fd = -1;
    slot->tls = NULL;
    return HAL_NET_OK;
}

int hal_net_udp_bind(uint16_t port, hal_net_udp_socket_t * out) {
    if (!out) return HAL_NET_ERR;
    *out = 0;
    esp32_net_init_tables();

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0) return HAL_NET_ERR;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        esp32_net_set_nonblock(fd, 1) != HAL_NET_OK) {
        close(fd);
        return HAL_NET_ERR;
    }

    uint16_t handle = esp32_net_alloc(udp_socks, ESP32_NET_MAX_UDP_SOCKS, fd);
    if (!handle) {
        close(fd);
        return HAL_NET_ERR;
    }
    *out = handle;
    return HAL_NET_OK;
}

int hal_net_udp_close(hal_net_udp_socket_t sock) {
    return esp32_net_close_slot(udp_socks, ESP32_NET_MAX_UDP_SOCKS, sock);
}

int hal_net_udp_socket_send(hal_net_udp_socket_t sock, const char * host,
                            uint16_t port, const void * buf, size_t len,
                            uint32_t timeout_ms) {
    if (!host || (!buf && len)) return HAL_NET_ERR;
    int * slot = esp32_net_slot(udp_socks, ESP32_NET_MAX_UDP_SOCKS, sock);
    if (!slot) return HAL_NET_ERR;

    struct addrinfo * ai = NULL;
    if (esp32_net_resolve_addr(host, port, SOCK_DGRAM, &ai) != HAL_NET_OK)
        return HAL_NET_ERR;

    int result = HAL_NET_ERR;
    for (struct addrinfo * rp = ai; rp; rp = rp->ai_next) {
        int wait = esp32_net_wait_fd(*slot, 1, timeout_ms);
        if (wait == HAL_NET_OK || wait == HAL_NET_TIMEOUT) {
            ssize_t n = sendto(*slot, buf, len, 0, rp->ai_addr, rp->ai_addrlen);
            if (n == (ssize_t)len) result = HAL_NET_OK;
        }
        if (result == HAL_NET_OK) break;
    }
    freeaddrinfo(ai);
    return result;
}

int hal_net_udp_send(const char * host, uint16_t port,
                     const void * buf, size_t len, uint32_t timeout_ms) {
    if (!host || (!buf && len)) return HAL_NET_ERR;
    struct addrinfo * ai = NULL;
    if (esp32_net_resolve_addr(host, port, SOCK_DGRAM, &ai) != HAL_NET_OK)
        return HAL_NET_ERR;

    int result = HAL_NET_ERR;
    for (struct addrinfo * rp = ai; rp; rp = rp->ai_next) {
        int fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes);
        int wait = esp32_net_wait_fd(fd, 1, timeout_ms);
        if (wait == HAL_NET_OK || wait == HAL_NET_TIMEOUT) {
            ssize_t n = sendto(fd, buf, len, 0, rp->ai_addr, rp->ai_addrlen);
            if (n == (ssize_t)len) result = HAL_NET_OK;
        }
        close(fd);
        if (result == HAL_NET_OK) break;
    }
    freeaddrinfo(ai);
    return result;
}

int hal_net_udp_recv_event(hal_net_udp_socket_t sock, hal_net_addr_t * from,
                           void * buf, size_t cap, size_t * len) {
    if (len) *len = 0;
    if (from) memset(from, 0, sizeof *from);
    int * slot = esp32_net_slot(udp_socks, ESP32_NET_MAX_UDP_SOCKS, sock);
    if (!slot || !buf || cap == 0) return HAL_NET_ERR;

    struct sockaddr_storage src;
    socklen_t src_len = sizeof src;
    ssize_t n = recvfrom(*slot, buf, cap, 0, (struct sockaddr *)&src, &src_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return HAL_NET_WOULD_BLOCK;
        return HAL_NET_ERR;
    }

    if (len) *len = (size_t)n;
    if (from && src.ss_family == AF_INET) {
        struct sockaddr_in * sin = (struct sockaddr_in *)&src;
        from->family = 4;
        from->port = ntohs(sin->sin_port);
        memcpy(from->bytes, &sin->sin_addr, 4);
    }
#if CONFIG_LWIP_IPV6
    else if (from && src.ss_family == AF_INET6) {
        struct sockaddr_in6 * sin6 = (struct sockaddr_in6 *)&src;
        from->family = 6;
        from->port = ntohs(sin6->sin6_port);
        memcpy(from->bytes, &sin6->sin6_addr, 16);
    }
#endif
    return HAL_NET_OK;
}

static int esp32_mqtt_put_remaining(uint8_t * out, size_t cap, size_t len,
                                    size_t * used) {
    size_t n = 0;
    do {
        if (n >= cap) return HAL_NET_ERR;
        uint8_t byte = (uint8_t)(len % 128);
        len /= 128;
        if (len) byte |= 0x80;
        out[n++] = byte;
    } while (len);
    if (used) *used = n;
    return HAL_NET_OK;
}

static int esp32_mqtt_put_utf8(uint8_t * out, size_t cap, size_t * pos,
                               const char * value) {
    size_t len = value ? strlen(value) : 0;
    if (len > 65535 || *pos + 2 + len > cap) return HAL_NET_ERR;
    out[(*pos)++] = (uint8_t)(len >> 8);
    out[(*pos)++] = (uint8_t)len;
    if (len) {
        memcpy(out + *pos, value, len);
        *pos += len;
    }
    return HAL_NET_OK;
}

static int esp32_mqtt_send_packet(esp32_net_mqtt_slot_t * slot,
                                  const uint8_t * packet, size_t len,
                                  uint32_t timeout_ms) {
    if (!slot || !slot->plain_tcp) return HAL_NET_ERR;
    return hal_net_tcp_client_send(slot->plain_tcp, packet, len,
                                   timeout_ms ? timeout_ms : 5000);
}

static int esp32_mqtt_recv_exact(esp32_net_mqtt_slot_t * slot, uint8_t * buf,
                                 size_t len, uint32_t timeout_ms) {
    size_t got_total = 0;
    while (got_total < len) {
        size_t got = 0;
        int rc = hal_net_tcp_client_recv(slot->plain_tcp, buf + got_total,
                                         len - got_total, &got, timeout_ms);
        if (rc != HAL_NET_OK || got == 0) return rc == HAL_NET_OK ? HAL_NET_TIMEOUT : rc;
        got_total += got;
        timeout_ms = 200;
    }
    return HAL_NET_OK;
}

static int esp32_mqtt_recv_packet(esp32_net_mqtt_slot_t * slot,
                                  uint8_t * packet, size_t cap,
                                  size_t * packet_len,
                                  uint32_t timeout_ms) {
    if (packet_len) *packet_len = 0;
    if (!slot || !slot->plain_tcp || !packet || cap < 2) return HAL_NET_ERR;
    int rc = esp32_mqtt_recv_exact(slot, packet, 1, timeout_ms);
    if (rc != HAL_NET_OK) return rc;
    size_t pos = 1;
    size_t remaining = 0;
    size_t multiplier = 1;
    uint8_t byte = 0;
    do {
        if (pos >= cap || multiplier > 128 * 128 * 128) return HAL_NET_ERR;
        rc = esp32_mqtt_recv_exact(slot, &byte, 1, timeout_ms);
        if (rc != HAL_NET_OK) return rc;
        packet[pos++] = byte;
        remaining += (size_t)(byte & 0x7f) * multiplier;
        multiplier *= 128;
    } while (byte & 0x80);
    if (pos + remaining > cap) return HAL_NET_ERR;
    rc = esp32_mqtt_recv_exact(slot, packet + pos, remaining, timeout_ms);
    if (rc != HAL_NET_OK) return rc;
    pos += remaining;
    if (packet_len) *packet_len = pos;
    return HAL_NET_OK;
}

static void esp32_mqtt_store_publish(esp32_net_mqtt_slot_t * slot,
                                     const uint8_t * packet, size_t len) {
    if (!slot || !packet || len < 4 || (packet[0] >> 4) != 3) return;
    size_t pos = 1;
    size_t remaining = 0;
    size_t multiplier = 1;
    uint8_t byte = 0;
    do {
        if (pos >= len) return;
        byte = packet[pos++];
        remaining += (size_t)(byte & 0x7f) * multiplier;
        multiplier *= 128;
    } while (byte & 0x80);
    if (pos + remaining > len || pos + 2 > len) return;
    size_t topic_len = ((size_t)packet[pos] << 8) | packet[pos + 1];
    pos += 2;
    if (pos + topic_len > len) return;
    size_t topic_copy = topic_len < sizeof slot->topic - 1 ? topic_len : sizeof slot->topic - 1;
    memcpy(slot->topic, packet + pos, topic_copy);
    slot->topic[topic_copy] = 0;
    pos += topic_len;
    size_t payload_len = len - pos;
    size_t payload_copy = payload_len < sizeof slot->payload ? payload_len : sizeof slot->payload;
    memcpy(slot->payload, packet + pos, payload_copy);
    slot->payload_len = payload_copy;
    slot->pending = 1;
}

static int esp32_mqtt_plain_connect(esp32_net_mqtt_slot_t * slot,
                                    const char * host, uint16_t port,
                                    const char * user, const char * pass,
                                    uint32_t timeout_ms) {
    if (hal_net_tcp_client_open(host, port, timeout_ms ? timeout_ms : 5000, 0,
                                &slot->plain_tcp) != HAL_NET_OK)
        return HAL_NET_TIMEOUT;

    uint8_t body[256];
    size_t pos = 0;
    if (esp32_mqtt_put_utf8(body, sizeof body, &pos, "MQTT") != HAL_NET_OK)
        return HAL_NET_ERR;
    body[pos++] = 4;
    uint8_t flags = 0x02;
    if (user && *user) flags |= 0x80;
    if (pass && *pass) flags |= 0x40;
    body[pos++] = flags;
    body[pos++] = 0;
    body[pos++] = 100;
    if (esp32_mqtt_put_utf8(body, sizeof body, &pos, slot->client_id) != HAL_NET_OK)
        return HAL_NET_ERR;
    if (user && *user && esp32_mqtt_put_utf8(body, sizeof body, &pos, user) != HAL_NET_OK)
        return HAL_NET_ERR;
    if (pass && *pass && esp32_mqtt_put_utf8(body, sizeof body, &pos, pass) != HAL_NET_OK)
        return HAL_NET_ERR;

    uint8_t packet[300];
    size_t rem_len = 0;
    packet[0] = 0x10;
    if (esp32_mqtt_put_remaining(packet + 1, sizeof packet - 1, pos,
                                 &rem_len) != HAL_NET_OK)
        return HAL_NET_ERR;
    if (1 + rem_len + pos > sizeof packet) return HAL_NET_ERR;
    memcpy(packet + 1 + rem_len, body, pos);
    if (esp32_mqtt_send_packet(slot, packet, 1 + rem_len + pos, timeout_ms) != HAL_NET_OK)
        return HAL_NET_ERR;

    return HAL_NET_OK;
}

int hal_net_mqtt_connect(const char * host, uint16_t port, const char * user,
                         const char * pass, const char * client_id, int tls,
                         uint32_t timeout_ms, hal_net_mqtt_client_t * out) {
    if (out) *out = 0;
    if (!host || !out) return HAL_NET_ERR;

    hal_net_mqtt_client_t handle = esp32_net_alloc_mqtt_slot();
    if (!handle) return HAL_NET_ERR;
    esp32_net_mqtt_slot_t * slot = &mqtt_clients[handle - 1];

    esp32_net_copy_string(slot->host, sizeof slot->host, host);
    esp32_net_copy_string(slot->user, sizeof slot->user, user);
    esp32_net_copy_string(slot->pass, sizeof slot->pass, pass);
    if (client_id && *client_id) {
        esp32_net_copy_string(slot->client_id, sizeof slot->client_id, client_id);
    } else {
        esp32_net_mqtt_make_client_id(slot->client_id, sizeof slot->client_id);
    }
    if (!tls) {
        int rc = esp32_mqtt_plain_connect(slot, slot->host, port, slot->user,
                                          slot->pass, timeout_ms);
        if (rc != HAL_NET_OK) {
            if (slot->plain_tcp) hal_net_tcp_client_close(slot->plain_tcp);
            memset(slot, 0, sizeof *slot);
            return rc;
        }
        slot->connected = 1;
        slot->packet_id = 1;
        *out = handle;
        return HAL_NET_OK;
    }
    esp_mqtt_client_config_t cfg = {
        .broker.address.hostname = slot->host,
        .broker.address.port = (uint32_t)port,
        .broker.address.transport = tls ? MQTT_TRANSPORT_OVER_SSL
                                        : MQTT_TRANSPORT_OVER_TCP,
        .broker.verification.certificate = tls && tls_ca_pem ? tls_ca_pem : NULL,
        /* PEM length must include the terminating NUL — the same +1 the
         * esp-tls client path uses. Without it esp-mqtt's mbedTLS PEM
         * parse rejects the CA and the handshake fails to open. */
        .broker.verification.certificate_len =
            tls && tls_ca_pem ? (tls_ca_len + 1) : 0,
        .broker.verification.crt_bundle_attach =
            tls && !tls_ca_pem ? esp_crt_bundle_attach : NULL,
        .credentials.username = *slot->user ? slot->user : NULL,
        .credentials.client_id = slot->client_id,
        .credentials.authentication.password = *slot->pass ? slot->pass : NULL,
        .network.disable_auto_reconnect = true,
        .network.timeout_ms = timeout_ms ? (int)timeout_ms : 5000,
        .session.keepalive = 100,
#if CONFIG_IDF_TARGET_ESP32
        .task.stack_size = 4096,
        .buffer.size = 256,
        .buffer.out_size = 256,
#endif
    };

    /* MQTT-over-TLS spawns its own task and runs the mbedTLS handshake
     * from internal/DMA SRAM alongside Wi-Fi. The native VGA scanout holds
     * 76.8 KB of that SRAM for the life of the session, leaving too little
     * for the TLS transport to come up — it fails late with a socket
     * select() timeout and a flood of Wi-Fi buffer warnings. Detect the
     * unwinnable case up front and fail cleanly instead. Non-TLS MQTT,
     * TLS HTTP, and TLS streaming all fit and are unaffected. */
    if (tls && vga_lcdcam_s3_scanout_reserved()) {
        ESP_LOGE("hal_net",
                 "MQTT-over-TLS needs internal RAM the VGA scanout reserves; "
                 "OPTION VGA DISABLE (reboot) to use it");
        memset(slot, 0, sizeof *slot);
        return HAL_NET_NOMEM;
    }
    slot->client = esp_mqtt_client_init(&cfg);
    if (!slot->client) {
        memset(slot, 0, sizeof *slot);
        return HAL_NET_ERR;
    }
    esp_mqtt_client_register_event(slot->client, MQTT_EVENT_ANY,
                                   esp32_net_mqtt_event_handler, slot);
    if (esp_mqtt_client_start(slot->client) != ESP_OK) {
        esp_mqtt_client_destroy(slot->client);
        memset(slot, 0, sizeof *slot);
        return HAL_NET_ERR;
    }
    int rc = esp32_net_wait_flag(&slot->connected,
                                 timeout_ms ? timeout_ms : 5000);
    if (rc != HAL_NET_OK) {
        esp_mqtt_client_stop(slot->client);
        esp_mqtt_client_destroy(slot->client);
        memset(slot, 0, sizeof *slot);
        return rc;
    }
    *out = handle;
    return HAL_NET_OK;
}

int hal_net_mqtt_publish(hal_net_mqtt_client_t client, const char * topic,
                         const void * payload, size_t len, int qos, int retain) {
    esp32_net_mqtt_slot_t * slot = esp32_net_mqtt_slot(client);
    if (!slot || !slot->connected || !topic || (!payload && len)) return HAL_NET_ERR;
    if (slot->plain_tcp) {
        if (qos != 0) return HAL_NET_UNSUPPORTED;
        uint8_t body[512];
        size_t pos = 0;
        if (esp32_mqtt_put_utf8(body, sizeof body, &pos, topic) != HAL_NET_OK)
            return HAL_NET_ERR;
        if (pos + len > sizeof body) return HAL_NET_ERR;
        if (len) memcpy(body + pos, payload, len);
        pos += len;
        uint8_t packet[540];
        size_t rem_len = 0;
        packet[0] = retain ? 0x31 : 0x30;
        if (esp32_mqtt_put_remaining(packet + 1, sizeof packet - 1, pos,
                                     &rem_len) != HAL_NET_OK)
            return HAL_NET_ERR;
        if (1 + rem_len + pos > sizeof packet) return HAL_NET_ERR;
        memcpy(packet + 1 + rem_len, body, pos);
        return esp32_mqtt_send_packet(slot, packet, 1 + rem_len + pos, 5000);
    }
    int id = esp_mqtt_client_publish(slot->client, topic, (const char *)payload,
                                     (int)len, qos, retain);
    return id < 0 ? HAL_NET_ERR : HAL_NET_OK;
}

int hal_net_mqtt_subscribe(hal_net_mqtt_client_t client, const char * topic,
                           int qos, uint32_t timeout_ms) {
    esp32_net_mqtt_slot_t * slot = esp32_net_mqtt_slot(client);
    if (!slot || !slot->connected || !topic) return HAL_NET_ERR;
    if (slot->plain_tcp) {
        if (qos != 0) return HAL_NET_UNSUPPORTED;
        uint8_t body[300];
        size_t pos = 0;
        uint16_t id = ++slot->packet_id;
        if (!id) id = ++slot->packet_id;
        body[pos++] = (uint8_t)(id >> 8);
        body[pos++] = (uint8_t)id;
        if (esp32_mqtt_put_utf8(body, sizeof body, &pos, topic) != HAL_NET_OK)
            return HAL_NET_ERR;
        body[pos++] = 0;
        uint8_t packet[320];
        size_t rem_len = 0;
        packet[0] = 0x82;
        if (esp32_mqtt_put_remaining(packet + 1, sizeof packet - 1, pos,
                                     &rem_len) != HAL_NET_OK)
            return HAL_NET_ERR;
        if (1 + rem_len + pos > sizeof packet) return HAL_NET_ERR;
        memcpy(packet + 1 + rem_len, body, pos);
        if (esp32_mqtt_send_packet(slot, packet, 1 + rem_len + pos,
                                   timeout_ms) != HAL_NET_OK)
            return HAL_NET_ERR;
        uint8_t resp[512];
        size_t resp_len = 0;
        int rc = HAL_NET_ERR;
        for (int i = 0; i < 3; ++i) {
            rc = esp32_mqtt_recv_packet(slot, resp, sizeof resp, &resp_len,
                                        timeout_ms ? timeout_ms : 4000);
            if (rc != HAL_NET_OK) return HAL_NET_ERR;
            if (resp_len >= 5 && resp[0] == 0x90) break;
            if (resp_len >= 4 && resp[0] == 0x20) continue;
            esp32_mqtt_store_publish(slot, resp, resp_len);
        }
        if (resp_len < 5 || resp[0] != 0x90) return HAL_NET_ERR;
        rc = esp32_mqtt_recv_packet(slot, resp, sizeof resp, &resp_len, 200);
        if (rc == HAL_NET_OK) esp32_mqtt_store_publish(slot, resp, resp_len);
        return HAL_NET_OK;
    }
    slot->subscribed = 0;
    if (esp_mqtt_client_subscribe(slot->client, topic, qos) < 0)
        return HAL_NET_ERR;
    return esp32_net_wait_flag(&slot->subscribed,
                               timeout_ms ? timeout_ms : 4000);
}

int hal_net_mqtt_unsubscribe(hal_net_mqtt_client_t client, const char * topic,
                             uint32_t timeout_ms) {
    esp32_net_mqtt_slot_t * slot = esp32_net_mqtt_slot(client);
    if (!slot || !slot->connected || !topic) return HAL_NET_ERR;
    if (slot->plain_tcp) {
        uint8_t body[300];
        size_t pos = 0;
        uint16_t id = ++slot->packet_id;
        if (!id) id = ++slot->packet_id;
        body[pos++] = (uint8_t)(id >> 8);
        body[pos++] = (uint8_t)id;
        if (esp32_mqtt_put_utf8(body, sizeof body, &pos, topic) != HAL_NET_OK)
            return HAL_NET_ERR;
        uint8_t packet[320];
        size_t rem_len = 0;
        packet[0] = 0xa2;
        if (esp32_mqtt_put_remaining(packet + 1, sizeof packet - 1, pos,
                                     &rem_len) != HAL_NET_OK)
            return HAL_NET_ERR;
        if (1 + rem_len + pos > sizeof packet) return HAL_NET_ERR;
        memcpy(packet + 1 + rem_len, body, pos);
        if (esp32_mqtt_send_packet(slot, packet, 1 + rem_len + pos,
                                   timeout_ms) != HAL_NET_OK)
            return HAL_NET_ERR;
        uint8_t resp[16];
        size_t resp_len = 0;
        int rc = esp32_mqtt_recv_packet(slot, resp, sizeof resp, &resp_len,
                                        timeout_ms ? timeout_ms : 4000);
        return rc == HAL_NET_OK && resp_len >= 4 && resp[0] == 0xb0
                   ? HAL_NET_OK
                   : HAL_NET_ERR;
    }
    slot->unsubscribed = 0;
    if (esp_mqtt_client_unsubscribe(slot->client, topic) < 0)
        return HAL_NET_ERR;
    return esp32_net_wait_flag(&slot->unsubscribed,
                               timeout_ms ? timeout_ms : 4000);
}

int hal_net_mqtt_recv_event(hal_net_mqtt_client_t client, char * topic,
                            size_t topic_cap, void * payload,
                            size_t payload_cap, size_t * payload_len) {
    esp32_net_mqtt_slot_t * slot = esp32_net_mqtt_slot(client);
    if (topic && topic_cap) topic[0] = 0;
    if (payload_len) *payload_len = 0;
    if (!slot || !payload || payload_cap == 0) return HAL_NET_ERR;
    if (slot->plain_tcp && !slot->pending) {
        uint8_t packet[512];
        size_t packet_len = 0;
        int rc = esp32_mqtt_recv_packet(slot, packet, sizeof packet,
                                        &packet_len, 1);
        if (rc == HAL_NET_OK) esp32_mqtt_store_publish(slot, packet, packet_len);
    }
    if (!slot->pending) return HAL_NET_WOULD_BLOCK;
    if (topic && topic_cap) {
        strncpy(topic, slot->topic, topic_cap - 1);
        topic[topic_cap - 1] = 0;
    }
    size_t copy = slot->payload_len < payload_cap ? slot->payload_len : payload_cap;
    memcpy(payload, slot->payload, copy);
    if (payload_len) *payload_len = copy;
    slot->pending = 0;
    return HAL_NET_OK;
}

int hal_net_mqtt_close(hal_net_mqtt_client_t client) {
    esp32_net_mqtt_slot_t * slot = esp32_net_mqtt_slot(client);
    if (!slot) return HAL_NET_ERR;
    if (slot->plain_tcp) {
        const uint8_t disconnect[] = {0xe0, 0x00};
        esp32_mqtt_send_packet(slot, disconnect, sizeof disconnect, 1000);
        hal_net_tcp_client_close(slot->plain_tcp);
        memset(slot, 0, sizeof *slot);
        return HAL_NET_OK;
    }
    esp_mqtt_client_stop(slot->client);
    esp_mqtt_client_destroy(slot->client);
    memset(slot, 0, sizeof *slot);
    return HAL_NET_OK;
}
