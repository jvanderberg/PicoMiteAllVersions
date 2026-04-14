#ifndef VM_DEVICE_FILEIO_H
#define VM_DEVICE_FILEIO_H

#ifdef PICOMITE_VM_DEVICE_ONLY

#include <stdbool.h>

void vm_device_storage_init(void);
void vm_device_print_files(const char *spec);
void vm_device_print_cwd(void);
bool vm_device_set_drive_spec(const char *spec);
bool vm_device_run_program(const char *spec);
bool vm_device_program_is_loaded(void);
const char *vm_device_program_name(void);
void vm_device_new_program(void);
bool vm_device_load_program(const char *spec);
bool vm_device_save_program(const char *spec);
void vm_device_list_program(const char *spec, bool all);
void vm_device_print_memory_report(void);
void vm_device_help(const char *topic);

#endif

#endif
