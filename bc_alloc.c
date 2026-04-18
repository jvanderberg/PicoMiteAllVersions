/*
 * bc_alloc.c - VM allocator wrapper.
 *
 * Two build paths:
 *   - Host (MMBASIC_HOST): calloc/free for off-device testing.
 *   - Device: TryGetMemory/FreeMemory sharing the interpreter's page-based
 *     MMHeap.  No separate arena — both interpreter and VM use one allocator.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bytecode.h"
#include "bc_alloc.h"

#ifdef MMBASIC_HOST

#include <stdlib.h>
#include <stdio.h>

/*
 * Host memory simulation — tracks allocations against a device heap budget.
 *
 * Every bc_alloc() rounds up to PAGESIZE (256 bytes) to match device
 * granularity, stores the rounded size in a hidden header, and charges it
 * against the heap budget.  Returns NULL when the budget would be exceeded,
 * exactly like TryGetMemory() does on device.
 *
 * Default budget: 128 KB (RP2040).  Override with env var BC_HEAP_LIMIT
 * (in bytes), e.g. BC_HEAP_LIMIT=307200 for RP2350 300 KB.
 */

#define HOST_PAGESIZE 256
#define HOST_ROUNDUP(n) (((n) + (HOST_PAGESIZE - 1)) & ~((size_t)(HOST_PAGESIZE - 1)))

/* Default heap budget matches the simulated device profile */
#if defined(BC_SIM_RP2040)
  #define HOST_DEFAULT_HEAP (128 * 1024)
#elif defined(BC_SIM_RP2350)
  #define HOST_DEFAULT_HEAP (300 * 1024)
#else
  #define HOST_DEFAULT_HEAP 0  /* 0 = unlimited (no simulation) */
#endif

static size_t heap_capacity  = 0;       /* set on first alloc or reset */
static size_t heap_used      = 0;
static size_t heap_high_water = 0;
static int    heap_inited    = 0;

/* Hidden header before every returned pointer */
typedef struct {
    size_t aligned_size;  /* page-rounded size charged to budget */
} AllocHeader;

static void heap_init_once(void) {
    if (heap_inited) return;
    heap_inited = 1;
    heap_capacity = HOST_DEFAULT_HEAP;
    const char *env = getenv("BC_HEAP_LIMIT");
    if (env) {
        size_t v = (size_t)strtoull(env, NULL, 0);
        if (v > 0) heap_capacity = v;
    }
}

void *bc_alloc(size_t size) {
    heap_init_once();
    if (size == 0) size = 1;
    size_t aligned = HOST_ROUNDUP(size);
    if (heap_capacity > 0 && heap_used + aligned > heap_capacity) {
        fprintf(stderr, "[bc_alloc] OOM: need %zu, used %zu/%zu\n",
                aligned, heap_used, heap_capacity);
        return NULL;
    }
    AllocHeader *hdr = calloc(1, sizeof(AllocHeader) + aligned);
    if (!hdr) return NULL;
    hdr->aligned_size = aligned;
    heap_used += aligned;
    if (heap_used > heap_high_water) heap_high_water = heap_used;
    return (void *)(hdr + 1);
}

void bc_free(void *ptr) {
    if (!ptr) return;
    AllocHeader *hdr = ((AllocHeader *)ptr) - 1;
    heap_used -= hdr->aligned_size;
    free(hdr);
}

void bc_alloc_reset(void) {
    heap_used = 0;
    heap_high_water = 0;
    heap_inited = 0;
}

/* Runtime override of the VM heap budget. Called from host_wasm_main.c's
 * wasm_set_heap_size so the interpreter MMHeap and the VM heap resize
 * together on the web host. 0 = unlimited. */
void bc_alloc_set_heap_capacity(size_t bytes) {
    heap_inited = 1;
    heap_capacity = bytes;
}

void *bc_compile_alloc(size_t size) {
    return bc_alloc(size);
}

void bc_compile_free(void *ptr) {
    bc_free(ptr);
}

void bc_compile_release_all(void) {
}

int bc_compile_owns(const void *ptr) {
    (void)ptr;
    return 0;
}

size_t bc_alloc_bytes_used(void)        { return heap_used; }
size_t bc_alloc_bytes_high_water(void)  { return heap_high_water; }
size_t bc_alloc_bytes_capacity(void)    { heap_init_once(); return heap_capacity; }
size_t bc_alloc_usable_size(void *ptr) {
    if (!ptr) return 0;
    AllocHeader *hdr = ((AllocHeader *)ptr) - 1;
    return hdr->aligned_size;
}
int bc_alloc_owns(const void *ptr)      { (void)ptr; return 0; }
size_t bc_alloc_bytes_used_peek(void)   { return heap_used; }
size_t bc_alloc_bytes_high_water_peek(void) { return heap_high_water; }
size_t bc_compile_bytes_used(void)      { return 0; }
size_t bc_compile_bytes_free(void)      { return 0; }
size_t bc_runtime_bytes_limit(void)     { heap_init_once(); return heap_capacity; }

#else

/*
 * Device build: VM allocations use the interpreter's page-based heap (MMHeap)
 * via TryGetMemory/FreeMemory.  No separate arena — both sides share one
 * allocator.
 */

#include "vm_device_support.h"

void *bc_alloc(size_t size) {
    if (size == 0) size = 1;
    return TryGetMemory((int)size);  /* returns NULL on OOM; already zeroed */
}

void bc_free(void *ptr) {
    if (ptr) FreeMemory((unsigned char *)ptr);
}

void bc_alloc_reset(void) {
    /* no-op — interpreter owns heap lifecycle via InitHeap */
}

void *bc_compile_alloc(size_t size) {
    return bc_alloc(size);
}

void bc_compile_free(void *ptr) {
    bc_free(ptr);
}

void bc_compile_release_all(void) {
    /* no-op — individual frees handle cleanup */
}

int bc_compile_owns(const void *ptr) {
    (void)ptr;
    return 0;  /* no separate compile arena */
}

size_t bc_alloc_bytes_used(void) { return 0; }
size_t bc_alloc_bytes_high_water(void) { return 0; }
size_t bc_alloc_bytes_capacity(void) { return 0; }
size_t bc_alloc_usable_size(void *ptr) { (void)ptr; return 0; }
int bc_alloc_owns(const void *ptr) { (void)ptr; return 0; }
size_t bc_alloc_bytes_used_peek(void) { return 0; }
size_t bc_alloc_bytes_high_water_peek(void) { return 0; }
size_t bc_compile_bytes_used(void) { return 0; }
size_t bc_compile_bytes_free(void) { return 0; }
size_t bc_runtime_bytes_limit(void) { return 0; }

#endif
