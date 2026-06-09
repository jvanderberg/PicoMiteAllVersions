/*
 * ports/esp32_s3/main/esp32_mbedtls_mem.c - mbedTLS allocator
 * (CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC).
 *
 * TLS sessions allocate from PSRAM when present, falling back to internal
 * RAM. Internal RAM is the scarce resource on this port — WiFi buffers,
 * the MMBasic heap, and the esp-mqtt client task coexist with mbedTLS,
 * and a TLS handshake (certificate chain, session structures, record
 * buffers) needs more contiguous space than is reliably left over.
 * PSRAM-resident TLS state is an officially supported ESP-IDF
 * configuration (MBEDTLS_EXTERNAL_MEM_ALLOC); the custom hook here only
 * adds the internal-RAM fallback so no-PSRAM boards keep working.
 *
 * CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC removes ESP-IDF's compile-time
 * MBEDTLS_PLATFORM_STD_CALLOC mapping; the allocator must be installed
 * at runtime with mbedtls_platform_set_calloc_free().
 * esp32_mbedtls_mem_install() is called from the WiFi bring-up path
 * (esp32_net_wifi_ensure_ready), which every TLS connection passes
 * through first. An explicit call rather than a constructor: nothing
 * else references this object file, and the linker drops
 * static-library objects whose symbols are never used — a constructor
 * here would silently never run.
 */

#include "esp_heap_caps.h"
#include "mbedtls/platform.h"

static void * esp32_tls_calloc(size_t n, size_t size) {
    return heap_caps_calloc_prefer(n, size, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void esp32_tls_free(void * ptr) {
    heap_caps_free(ptr);
}

void esp32_mbedtls_mem_install(void) {
    mbedtls_platform_set_calloc_free(esp32_tls_calloc, esp32_tls_free);
}
