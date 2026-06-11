/*
 * ports/esp32_s3/main/hal_psram_esp32.c - ESP32-S3 PSRAM HAL.
 *
 * Owns MMBasic's PSRAMsize / PSRAMbase globals on this port. At boot,
 * hal_psram_init():
 *
 *   1. Calls esp_psram_init() to bring up the ESP-IDF SPIRAM driver.
 *   2. Reserves a slab sized from the PSRAM actually detected (the largest
 *      free SPIRAM block, less a proportional reserve) via
 *      heap_caps_aligned_alloc(PAGESIZE, slab, MALLOC_CAP_SPIRAM |
 *      MALLOC_CAP_8BIT). PAGESIZE alignment matches the bitmap allocator
 *      page granularity in drivers/psram_heap/psram_heap_real.c. The slab
 *      follows the chip's capacity, so any size part (2/8/16 MB, ...) is
 *      handled by the same code with no per-board size constant. (PSRAM line
 *      mode — octal vs quad — is a separate, build-time choice in sdkconfig.)
 *   3. Publishes the slab pointer and heap size to PSRAMbase / PSRAMsize so
 *      Memory.c routing, the `RAM` command, and the bitmap allocator
 *      all see the same region.
 *
 * Failure modes are non-fatal: if esp_psram_init() or
 * heap_caps_aligned_alloc() fail, PSRAMbase / PSRAMsize stay 0 and the
 * `if (PSRAMsize)` guards in core code keep PSRAM paths dormant. A
 * one-line warning goes to the console so the operator can investigate.
 *
 * The other HAL hooks (cache_sync / nocache_alias / save_settings /
 * restore_settings) are wired against ESP-IDF expectations:
 *
 *   - cache_sync: clean + invalidate via esp_cache_msync over the slab.
 *   - nocache_alias: returns NULL — ESP32-S3 has no cache-bypass alias
 *     for PSRAM; the shared `RAM TEST NOCACHE` path translates NULL
 *     into the BASIC-visible "NOCACHE not supported on this port" error.
 *   - save/restore_settings: no-ops; ESP-IDF manages cache coherency
 *     around flash erase/program internally.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "hal/hal_psram.h"

#include "esp_err.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"

/* Strong definitions of the runtime PSRAM globals on this port.
 * PicoMite.c provides them on Pico builds; the ESP32 port has its own
 * non-PicoMite.c TU layout, so the responsibility lives here. */
uint32_t PSRAMsize = 0;
uintptr_t PSRAMbase = 0;

/* Slab handle retained for the lifetime of the program; freed only at
 * reset. cache_sync uses these to scope esp_cache_msync to exactly the
 * slab — no point invalidating ESP-IDF-owned SPIRAM regions. */
static void * s_psram_slab = NULL;
static size_t s_psram_slab_bytes = 0;

extern const unsigned int psmap_size_bytes;

void hal_psram_init(void) {
    if (s_psram_slab != NULL) return; /* idempotent */

    /* CONFIG_SPIRAM_BOOT_INIT=y in sdkconfig brings up PSRAM during the
     * ESP-IDF bootloader, so we only call esp_psram_init() if it hasn't
     * happened yet. Calling it twice returns ESP_ERR_INVALID_STATE. */
    if (!esp_psram_is_initialized()) {
        esp_err_t err = esp_psram_init();
        if (err != ESP_OK) {
            printf("hal_psram_init: esp_psram_init failed (%d); "
                   "PSRAM disabled\n",
                   (int)err);
            return;
        }
    }

    /*
     * Size the slab from the PSRAM that is actually present, not from a
     * per-board constant. Octal vs quad is a bus-width choice, not a size
     * (a board could pair either line mode with any capacity), so the heap
     * must follow the chip esp_psram_init() detected. Take the largest
     * contiguous SPIRAM block ESP-IDF leaves free, hold back a proportional
     * reserve for any later IDF/SPIRAM use, and page-align it.
     *
     * The slab covers the heap, a 0x60000 gap, then PSRAMblocksize bytes for
     * the numbered-slot region (shared formula PSRAMblock = PSRAMbase +
     * PSRAMsize + 0x60000). Only the heap portion is published as PSRAMsize,
     * so the bitmap allocator (drivers/psram_heap/psram_heap_real.c) and
     * Memory.c routing stay confined to it; the slots live in the slab tail
     * and are addressed linearly by cmd_psram.
     */
    const size_t overhead = 0x60000u + (size_t)HAL_PORT_PSRAM_BLOCK_SIZE;
    const size_t avail =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t reserve = avail / 8u;
    if (reserve < (size_t)HAL_PORT_PSRAM_RESERVE_MIN)
        reserve = (size_t)HAL_PORT_PSRAM_RESERVE_MIN;
    size_t slab_bytes = (avail > reserve) ? (avail - reserve) : 0u;
    slab_bytes &= ~((size_t)PAGESIZE - 1u); /* page-align down */
    if (slab_bytes <= overhead) {
        printf("hal_psram_init: only %u B SPIRAM available; PSRAM disabled\n",
               (unsigned)avail);
        return;
    }
    void * slab = heap_caps_aligned_alloc(PAGESIZE, slab_bytes,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!slab) {
        printf("hal_psram_init: heap_caps_aligned_alloc(%u) failed; "
               "PSRAM disabled\n",
               (unsigned)slab_bytes);
        return;
    }

    const size_t heap_capacity =
        ((size_t)psmap_size_bytes / sizeof(unsigned int)) *
        (size_t)PAGESPERWORD * (size_t)PAGESIZE;
    size_t heap_bytes = slab_bytes - overhead;
    if (heap_bytes > heap_capacity) heap_bytes = heap_capacity;
    s_psram_slab = slab;
    s_psram_slab_bytes = slab_bytes;
    PSRAMbase = (uintptr_t)slab;
    PSRAMsize = (uint32_t)heap_bytes;
    printf("hal_psram_init: spiram_free=%u slab_bytes=%u heap=%u "
           "PSRAMbase=0x%08lx PSRAMsize=%u\n",
           (unsigned)avail, (unsigned)slab_bytes,
           (unsigned)heap_bytes,
           (unsigned long)PSRAMbase, (unsigned)PSRAMsize);
}

void hal_psram_cache_sync(void) {
    if (!s_psram_slab) return;
    (void)esp_cache_msync(s_psram_slab, s_psram_slab_bytes,
                          ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                              ESP_CACHE_MSYNC_FLAG_INVALIDATE);
}

uint8_t * hal_psram_nocache_alias(uint8_t * base) {
    (void)base;
    return NULL; /* ESP32-S3 has no cache-bypass alias for PSRAM. */
}

void hal_psram_save_settings(void) {}
void hal_psram_restore_settings(void) {}
