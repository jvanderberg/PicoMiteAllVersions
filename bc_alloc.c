/*
 * bc_alloc.c - VM-owned allocator.
 *
 * This deliberately avoids GetMemory/GetTempMemory on device.  The bytecode
 * VM needs deterministic ownership so the interpreter heap can be removed
 * from the firmware path.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bytecode.h"
#include "bc_alloc.h"

#ifdef MMBASIC_HOST

#include <stdlib.h>

void *bc_alloc(size_t size) {
    if (size == 0) size = 1;
    return calloc(1, size);
}

void bc_free(void *ptr) {
    free(ptr);
}

void bc_alloc_reset(void) {
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

size_t bc_alloc_usable_size(void *ptr) {
    (void)ptr;
    return 0;
}

int bc_alloc_owns(const void *ptr) {
    (void)ptr;
    return 0;
}

#else

#ifndef BC_DEVICE_HEAP_SIZE
  #ifdef rp2350
    #define BC_DEVICE_HEAP_SIZE (232 * 1024)
  #else
    #define BC_DEVICE_HEAP_SIZE (256 * 1024)
  #endif
#endif

#define BC_BLOCK_MAGIC 0xBC0A110Cu

typedef struct BCBlockHeader {
    size_t size;
    struct BCBlockHeader *next;
    uint32_t magic;
    unsigned char free;
} BCBlockHeader;

static unsigned char bc_heap[BC_DEVICE_HEAP_SIZE] __attribute__((aligned(8)));
static BCBlockHeader *bc_free_list;
static size_t bc_heap_used;
static size_t bc_heap_high_water;

static size_t bc_align(size_t size) {
    return (size + 7u) & ~(size_t)7u;
}

void bc_alloc_reset(void) {
    memset(bc_heap, 0, sizeof(bc_heap));
    bc_free_list = (BCBlockHeader *)bc_heap;
    bc_free_list->size = sizeof(bc_heap) - sizeof(BCBlockHeader);
    bc_free_list->next = NULL;
    bc_free_list->magic = BC_BLOCK_MAGIC;
    bc_free_list->free = 1;
    bc_heap_used = 0;
    bc_heap_high_water = 0;
}

static void bc_alloc_init_if_needed(void) {
    if (!bc_free_list) bc_alloc_reset();
}

static void bc_split_block(BCBlockHeader *block, size_t size) {
    if (block->size < size + sizeof(BCBlockHeader) + 8) return;

    BCBlockHeader *next = (BCBlockHeader *)((unsigned char *)(block + 1) + size);
    next->size = block->size - size - sizeof(BCBlockHeader);
    next->next = block->next;
    next->magic = BC_BLOCK_MAGIC;
    next->free = 1;

    block->size = size;
    block->next = next;
}

void *bc_alloc(size_t size) {
    bc_alloc_init_if_needed();
    size = bc_align(size == 0 ? 1 : size);

    for (BCBlockHeader *block = bc_free_list; block; block = block->next) {
        if (!block->free || block->size < size) continue;

        bc_split_block(block, size);
        block->free = 0;
        bc_heap_used += block->size;
        if (bc_heap_used > bc_heap_high_water)
            bc_heap_high_water = bc_heap_used;

        void *ptr = (void *)(block + 1);
        memset(ptr, 0, block->size);
        return ptr;
    }

    return NULL;
}

static void bc_coalesce_free_blocks(void) {
    for (BCBlockHeader *block = bc_free_list; block && block->next; ) {
        if (block->free && block->next->free) {
            block->size += sizeof(BCBlockHeader) + block->next->size;
            block->next = block->next->next;
        } else {
            block = block->next;
        }
    }
}

void bc_free(void *ptr) {
    if (!ptr) return;

    BCBlockHeader *block = ((BCBlockHeader *)ptr) - 1;
    if ((unsigned char *)block < bc_heap ||
        (unsigned char *)block >= bc_heap + sizeof(bc_heap) ||
        block->magic != BC_BLOCK_MAGIC) {
        return;
    }

    if (!block->free) {
        block->free = 1;
        if (bc_heap_used >= block->size)
            bc_heap_used -= block->size;
        else
            bc_heap_used = 0;
        bc_coalesce_free_blocks();
    }
}

int bc_alloc_owns(const void *ptr) {
    return ptr &&
           (const unsigned char *)ptr >= bc_heap &&
           (const unsigned char *)ptr < bc_heap + sizeof(bc_heap);
}

size_t bc_alloc_usable_size(void *ptr) {
    if (!ptr) return 0;

    BCBlockHeader *block = ((BCBlockHeader *)ptr) - 1;
    if ((unsigned char *)block < bc_heap ||
        (unsigned char *)block >= bc_heap + sizeof(bc_heap) ||
        block->magic != BC_BLOCK_MAGIC ||
        block->free) {
        return 0;
    }

    return block->size;
}

size_t bc_alloc_bytes_used(void) {
    bc_alloc_init_if_needed();
    return bc_heap_used;
}

size_t bc_alloc_bytes_high_water(void) {
    bc_alloc_init_if_needed();
    return bc_heap_high_water;
}

size_t bc_alloc_bytes_capacity(void) {
    return sizeof(bc_heap);
}

#endif
