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
#include "vm_device_support.h"

/* Host bc_alloc — routes to TryGetMemory / FreeMemory so bc_alloc and
 * MMHeap share the AllMemory[] pool, exactly like the device. */

void *bc_alloc(size_t size) {
    if (size == 0) size = 1;
    return TryGetMemory((int)size);
}

void bc_free(void *ptr) {
    if (ptr) FreeMemory((unsigned char *)ptr);
}

void bc_alloc_reset(void) { }

void bc_alloc_set_heap_capacity(size_t bytes) { (void)bytes; }

void *bc_compile_alloc(size_t size) { return bc_alloc(size); }
void  bc_compile_free(void *ptr)    { bc_free(ptr); }
void  bc_compile_release_all(void)  {}
int   bc_compile_owns(const void *ptr) { (void)ptr; return 0; }

size_t bc_alloc_bytes_used(void)        { return 0; }
size_t bc_alloc_bytes_high_water(void)  { return 0; }
size_t bc_alloc_bytes_capacity(void)    { return 0; }
size_t bc_alloc_usable_size(void *ptr)  { (void)ptr; return 0; }
int    bc_alloc_owns(const void *ptr)   { (void)ptr; return 0; }
size_t bc_alloc_bytes_used_peek(void)   { return 0; }
size_t bc_alloc_bytes_high_water_peek(void) { return 0; }
size_t bc_compile_bytes_used(void)      { return 0; }
size_t bc_compile_bytes_free(void)      { return 0; }
size_t bc_runtime_bytes_limit(void)     { return 0; }

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
