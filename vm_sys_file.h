#ifndef __VM_SYS_FILE_H
#define __VM_SYS_FILE_H

#include <stdint.h>

#define VM_FILE_MODE_INPUT  1
#define VM_FILE_MODE_OUTPUT 2
#define VM_FILE_MODE_APPEND 3

void vm_sys_file_open(const char *filename, int fnbr, int mode);
void vm_sys_file_close(int fnbr);
void vm_sys_file_reset(void);
void vm_sys_file_print_buf(int fnbr, const char *buf, int len);
void vm_sys_file_print_str(int fnbr, const uint8_t *mstr);
void vm_sys_file_print_newline(int fnbr);
void vm_sys_file_line_input(int fnbr, uint8_t *dest);
void vm_sys_file_files(void);

#endif
