/* Stub for host build */
#ifndef _HARDWARE_FLASH_H
#define _HARDWARE_FLASH_H
#include <stdint.h>
#define FLASH_PAGE_SIZE 256
#define FLASH_SECTOR_SIZE 4096
/* On the device these hit real flash. On host they simulate flash against
 * the flash_prog_buf in host_main.c, so NEW / SAVE / program-memory edits
 * actually take effect. Implementations live in host_stubs_legacy.c. */
void flash_range_erase(uint32_t off, uint32_t count);
void flash_range_program(uint32_t off, const uint8_t *data, uint32_t count);
#endif
