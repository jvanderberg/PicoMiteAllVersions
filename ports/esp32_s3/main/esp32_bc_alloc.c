/*
 * esp32_bc_alloc.c - ESP32 bytecode allocator split.
 *
 * Runtime VM allocations stay in the 48 KB MMBasic heap via TryGetMemory.
 * Transient compiler tables use ESP-IDF heap so FRUN compilation does not
 * consume the runtime heap budget before compaction. Ports without enough
 * contiguous ESP-IDF heap can opt into a BASIC-heap fallback in port_config.h.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "bc_alloc.h"
#include "bytecode.h"
#include "Memory.h"
#include "vm_device_support.h"

extern unsigned int bc_alloc_fail_size;
extern unsigned int bc_alloc_fail_pages;
extern unsigned int bc_alloc_fail_used;
extern unsigned int bc_alloc_fail_free;
extern unsigned int bc_alloc_fail_longest;
extern unsigned int bc_alloc_fail_total;

typedef struct esp32_compile_alloc_node {
    void * ptr;
    size_t size;
    int basic_heap_owned;
    struct esp32_compile_alloc_node * next;
} esp32_compile_alloc_node_t;

static esp32_compile_alloc_node_t * s_compile_allocs;
static size_t s_compile_used;
static size_t s_compile_high_water;

static const uint32_t compile_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
static const uint32_t compile_fallback_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#define BC_COMPILE_BASIC_HEAP_ENABLED (0 + HAL_PORT_BC_COMPILE_USE_BASIC_HEAP)

static int compile_basic_heap_enabled(void) {
    return BC_COMPILE_BASIC_HEAP_ENABLED != 0;
}

static void esp32_note_compile_alloc_failure(size_t size) {
    size_t free_bytes = heap_caps_get_free_size(compile_caps);
    size_t largest = heap_caps_get_largest_free_block(compile_caps);
    size_t fallback_free = heap_caps_get_free_size(compile_fallback_caps);
    size_t fallback_largest = heap_caps_get_largest_free_block(compile_fallback_caps);
    free_bytes += fallback_free;
    if (fallback_largest > largest) largest = fallback_largest;
    bc_alloc_fail_size = (unsigned int)size;
    bc_alloc_fail_pages = (unsigned int)((size + PAGESIZE - 1) / PAGESIZE);
    bc_alloc_fail_used = (unsigned int)((s_compile_used + PAGESIZE - 1) / PAGESIZE);
    bc_alloc_fail_free = (unsigned int)(free_bytes / PAGESIZE);
    bc_alloc_fail_longest = (unsigned int)(largest / PAGESIZE);
    bc_alloc_fail_total = bc_alloc_fail_used + bc_alloc_fail_free;
}

static void * esp32_compile_calloc(size_t nmemb, size_t size) {
    void * ptr = heap_caps_calloc(nmemb, size, compile_caps);
    if (!ptr) ptr = heap_caps_calloc(nmemb, size, compile_fallback_caps);
    return ptr;
}

static void * esp32_compile_alloc_payload(size_t size, int * basic_heap_owned) {
    void * ptr = esp32_compile_calloc(1, size);
    if (ptr) {
        *basic_heap_owned = 0;
        return ptr;
    }
    if (!compile_basic_heap_enabled()) return NULL;
    ptr = TryGetMemory((int)size);
    if (ptr) *basic_heap_owned = 1;
    return ptr;
}

static void esp32_compile_free_payload(void * ptr, int basic_heap_owned) {
    if (!ptr) return;
    if (basic_heap_owned) {
        FreeMemory((unsigned char *)ptr);
    } else {
        heap_caps_free(ptr);
    }
}

void * bc_alloc(size_t size) {
    if (size == 0) size = 1;
    return TryGetMemory((int)size);
}

void bc_free(void * ptr) {
    if (ptr) FreeMemory((unsigned char *)ptr);
}

void bc_alloc_reset(void) {
    /* Interpreter owns the MMBasic heap lifecycle via InitHeap. */
}

void bc_alloc_set_heap_capacity(size_t bytes) {
    (void)bytes;
}

void * bc_compile_alloc(size_t size) {
    if (size == 0) size = 1;

    int basic_heap_owned = 0;
    void * ptr = esp32_compile_alloc_payload(size, &basic_heap_owned);
    if (!ptr) {
        esp32_note_compile_alloc_failure(size);
        return NULL;
    }

    esp32_compile_alloc_node_t * node =
        (esp32_compile_alloc_node_t *)esp32_compile_calloc(1, sizeof(*node));
    if (!node) {
        esp32_compile_free_payload(ptr, basic_heap_owned);
        esp32_note_compile_alloc_failure(sizeof(*node));
        return NULL;
    }

    node->ptr = ptr;
    node->size = size;
    node->basic_heap_owned = basic_heap_owned;
    node->next = s_compile_allocs;
    s_compile_allocs = node;
    s_compile_used += size;
    if (s_compile_used > s_compile_high_water) s_compile_high_water = s_compile_used;
    return ptr;
}

void bc_compile_free(void * ptr) {
    if (!ptr) return;

    esp32_compile_alloc_node_t ** link = &s_compile_allocs;
    while (*link) {
        esp32_compile_alloc_node_t * node = *link;
        if (node->ptr == ptr) {
            *link = node->next;
            if (s_compile_used >= node->size)
                s_compile_used -= node->size;
            else
                s_compile_used = 0;
            esp32_compile_free_payload(node->ptr, node->basic_heap_owned);
            heap_caps_free(node);
            return;
        }
        link = &node->next;
    }
}

void bc_compile_release_all(void) {
    while (s_compile_allocs) {
        esp32_compile_alloc_node_t * node = s_compile_allocs;
        s_compile_allocs = node->next;
        esp32_compile_free_payload(node->ptr, node->basic_heap_owned);
        heap_caps_free(node);
    }
    s_compile_used = 0;
}

int bc_compile_owns(const void * ptr) {
    for (esp32_compile_alloc_node_t * node = s_compile_allocs; node; node = node->next) {
        if (node->ptr == ptr) return 1;
    }
    return 0;
}

size_t bc_alloc_bytes_used(void) {
    return 0;
}
size_t bc_alloc_bytes_high_water(void) {
    return 0;
}
size_t bc_alloc_bytes_capacity(void) {
    return 0;
}
size_t bc_alloc_usable_size(void * ptr) {
    (void)ptr;
    return 0;
}
int bc_alloc_owns(const void * ptr) {
    return ptr >= (const void *)MMHeap &&
           ptr < (const void *)(MMHeap + heap_memory_size);
}
size_t bc_alloc_bytes_used_peek(void) {
    return 0;
}
size_t bc_alloc_bytes_high_water_peek(void) {
    return 0;
}
size_t bc_compile_bytes_used(void) {
    return s_compile_used;
}
size_t bc_compile_bytes_free(void) {
    return heap_caps_get_free_size(compile_caps) +
           heap_caps_get_free_size(compile_fallback_caps);
}
size_t bc_runtime_bytes_limit(void) {
    return heap_memory_size;
}
