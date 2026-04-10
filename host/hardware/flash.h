/* Stub for host build */
#ifndef _HARDWARE_FLASH_H
#define _HARDWARE_FLASH_H
#include <stdint.h>
#define FLASH_PAGE_SIZE 256
#define FLASH_SECTOR_SIZE 4096
static inline void flash_range_erase(uint32_t off, uint32_t count) { (void)off; (void)count; }
static inline void flash_range_program(uint32_t off, const uint8_t *data, uint32_t count) { (void)off; (void)data; (void)count; }
#endif
