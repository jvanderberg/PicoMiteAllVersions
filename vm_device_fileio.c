#ifdef PICOMITE_VM_DEVICE_ONLY

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"

#include "vm_device_support.h"
#include "diskio.h"
#include "bc_alloc.h"
#include "bytecode.h"
#include "vm_sys_file.h"

#define VM_FLASH_BLOCK_SIZE 4096

extern const uint8_t *flash_option_contents;
extern volatile BYTE SDCardStat;

static FATFS vm_fatfs;
static bool vm_flash_ready = false;
static bool vm_sd_ready = false;
static char vm_flash_read_buffer[256];
static char vm_flash_prog_buffer[256];
static char vm_flash_lookahead_buffer[256];
static char fullpathname[2][FF_MAX_LFN];
static char vm_loaded_program_name[FF_MAX_LFN];

int FatFSFileSystemSave = 0;
char filepath[2][FF_MAX_LFN] = {
    "/",
    "/"
};

static void vm_device_decode_drive_path(const char *input, int *fs_out, const char **path_out);
static void vm_device_build_open_path(int fs, const char *input, char *out);
static void vm_device_build_dir_path(int fs, const char *input, char *out);
static void vm_device_mount_flash(void);
static void vm_device_append_bas_extension(char *filename);

static int vm_fs_flash_read(const struct lfs_config *cfg, lfs_block_t block,
                            lfs_off_t off, void *buffer, lfs_size_t size) {
    uint32_t addr;
    assert(off % cfg->read_size == 0);
    assert(size % cfg->read_size == 0);
    assert(block < cfg->block_count);
    addr = XIP_BASE + RoundUpK4(TOP_OF_SYSTEM_FLASH) +
           (Option.modbuff ? 1024 * Option.modbuffsize : 0) +
           block * VM_FLASH_BLOCK_SIZE + off;
    memcpy(buffer, (const void *)addr, size);
    return 0;
}

static int vm_fs_flash_prog(const struct lfs_config *cfg, lfs_block_t block,
                            lfs_off_t off, const void *buffer, lfs_size_t size) {
    uint32_t addr;
    assert(off % cfg->prog_size == 0);
    assert(size % cfg->prog_size == 0);
    assert(block < cfg->block_count);
    addr = RoundUpK4(TOP_OF_SYSTEM_FLASH) +
           (Option.modbuff ? 1024 * Option.modbuffsize : 0) +
           block * VM_FLASH_BLOCK_SIZE + off;
    disable_interrupts_pico();
    flash_range_program(addr, buffer, size);
    enable_interrupts_pico();
    return 0;
}

static int vm_fs_flash_erase(const struct lfs_config *cfg, lfs_block_t block) {
    uint32_t block_addr;
    assert(block < cfg->block_count);
    block_addr = RoundUpK4(TOP_OF_SYSTEM_FLASH) +
                 (Option.modbuff ? 1024 * Option.modbuffsize : 0) +
                 block * VM_FLASH_BLOCK_SIZE;
    disable_interrupts_pico();
    flash_range_erase(block_addr, VM_FLASH_BLOCK_SIZE);
    enable_interrupts_pico();
    return 0;
}

static int vm_fs_flash_sync(const struct lfs_config *cfg) {
    (void)cfg;
    return 0;
}

struct lfs_config pico_lfs_cfg = {
    .read = vm_fs_flash_read,
    .prog = vm_fs_flash_prog,
    .erase = vm_fs_flash_erase,
    .sync = vm_fs_flash_sync,
    .read_size = 1,
    .prog_size = 256,
    .block_size = VM_FLASH_BLOCK_SIZE,
    .block_count = 0,
    .block_cycles = 500,
    .cache_size = 256,
    .lookahead_size = 256,
    .read_buffer = (void *)vm_flash_read_buffer,
    .prog_buffer = (void *)vm_flash_prog_buffer,
    .lookahead_buffer = (void *)vm_flash_lookahead_buffer,
};

static void vm_device_file_error(const char *msg, int err) {
    FSerror = err;
    error((char *)msg);
}

static void vm_device_program_set(const char *name) {
    if (name) {
        strncpy(vm_loaded_program_name, name, sizeof(vm_loaded_program_name) - 1);
        vm_loaded_program_name[sizeof(vm_loaded_program_name) - 1] = '\0';
    } else {
        vm_loaded_program_name[0] = '\0';
    }
}

static void vm_device_open_source_path(const char *spec, int *fs_out, char *open_path, char *display_name) {
    char filename[FF_MAX_LFN];
    int fs;

    if (spec == NULL || spec[0] == '\0') error("File name");
    if (strlen(spec) >= sizeof(filename)) error("RUN command line too long");
    strcpy(filename, spec);
    vm_device_append_bas_extension(filename);
    vm_device_decode_drive_path(filename, &fs, (const char **)&spec);
    vm_device_build_open_path(fs, filename, open_path);
    if (display_name) {
        strncpy(display_name, filename, FF_MAX_LFN - 1);
        display_name[FF_MAX_LFN - 1] = '\0';
    }
    if (fs_out) *fs_out = fs;
}

static void vm_device_canonical_program_spec(const char *spec, char *out) {
    char filename[FF_MAX_LFN];
    char path[FF_MAX_LFN];
    int fs;

    if (spec == NULL || spec[0] == '\0') error("File name");
    if (strlen(spec) >= sizeof(filename)) error("RUN command line too long");
    strcpy(filename, spec);
    vm_device_append_bas_extension(filename);
    vm_device_decode_drive_path(filename, &fs, (const char **)&spec);
    vm_device_build_dir_path(fs, filename, path);
    snprintf(out, FF_MAX_LFN, "%c:%s", fs ? 'B' : 'A', path);
}

static void vm_device_scan_source_file(const char *spec, int emit_text, size_t *len_out, int *lines_out) {
    char open_path[FF_MAX_LFN];
    int fs;
    size_t total = 0;
    int lines = 0;
    int saw_any = 0;
    int last_char = 0;

    vm_device_mount_flash();
    vm_device_open_source_path(spec, &fs, open_path, NULL);

    if (fs) {
        FIL file;
        UINT br = 0;
        unsigned char rbuf[256];

        FatFSFileSystem = FatFSFileSystemSave = 1;
        if (!InitSDCard()) error("SD Card not found");
        FSerror = f_open(&file, open_path, FA_READ);
        if (FSerror) vm_device_file_error("File error", FSerror);
        while (1) {
            FSerror = f_read(&file, rbuf, sizeof(rbuf), &br);
            if (FSerror) {
                f_close(&file);
                vm_device_file_error("File error", FSerror);
            }
            if (br == 0) break;
            for (UINT i = 0; i < br; ++i) {
                int c = rbuf[i] & 0x7f;
                if (isprint(c) || c == '\r' || c == '\n' || c == TAB) {
                    if (emit_text && c == '\n') {
                        MMPrintString("\r\n");
                        lines++;
                    } else if (emit_text && c != '\r') {
                        if (c == TAB) c = ' ';
                        MMputchar(c, 1);
                    } else if (!emit_text && c == '\n') {
                        lines++;
                    }
                    total++;
                    saw_any = 1;
                    last_char = c;
                }
            }
        }
        FSerror = f_close(&file);
        if (FSerror) vm_device_file_error("File error", FSerror);
    } else {
        lfs_file_t file;
        unsigned char rbuf[256];
        int rc;

        FatFSFileSystem = FatFSFileSystemSave = 0;
        FSerror = lfs_file_open(&lfs, &file, open_path, LFS_O_RDONLY);
        if (FSerror < 0) vm_device_file_error("File error", FSerror);
        while ((rc = lfs_file_read(&lfs, &file, rbuf, sizeof(rbuf))) > 0) {
            for (int i = 0; i < rc; ++i) {
                int c = rbuf[i] & 0x7f;
                if (isprint(c) || c == '\r' || c == '\n' || c == TAB) {
                    if (emit_text && c == '\n') {
                        MMPrintString("\r\n");
                        lines++;
                    } else if (emit_text && c != '\r') {
                        if (c == TAB) c = ' ';
                        MMputchar(c, 1);
                    } else if (!emit_text && c == '\n') {
                        lines++;
                    }
                    total++;
                    saw_any = 1;
                    last_char = c;
                }
            }
        }
        if (rc < 0) vm_device_file_error("File error", rc);
        FSerror = lfs_file_close(&lfs, &file);
        if (FSerror < 0) vm_device_file_error("File error", FSerror);
    }

    if (saw_any && last_char != '\n') lines++;
    if (len_out) *len_out = total;
    if (lines_out) *lines_out = saw_any ? lines : 0;
}

static void vm_device_reset_paths(void) {
    strcpy(filepath[0], "/");
    strcpy(filepath[1], "/");
    strcpy(fullpathname[0], "/");
    strcpy(fullpathname[1], "/");
}

static int vm_device_resolve_path(char *path, char *result, char *pos) {
    if (*path == '/') {
        *result = '/';
        pos = result + 1;
        path++;
    }
    *pos = 0;
    if (!*path) return 0;

    while (1) {
        char *slash = *path ? strchr(path, '/') : NULL;
        if (slash) *slash = 0;

        if (!path[0] || (path[0] == '.' && (!path[1] || (path[1] == '.' && !path[2])))) {
            pos--;
            if (pos != result && path[0] && path[1]) {
                while (*--pos != '/') {
                }
            }
        } else {
            strcpy(pos, path);
            pos = strchr(result, 0);
        }

        if (slash) {
            *pos++ = '/';
            path = slash + 1;
        }
        *pos = 0;
        if (!slash) break;
    }
    return 0;
}

static void vm_device_fullpath(int fs, const char *in, char *out) {
    char path[FF_MAX_LFN];
    char resolved[FF_MAX_LFN];
    const char *base = filepath[fs];

    strncpy(path, in, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    if (strcmp(path, ".") == 0 || path[0] == '\0') {
        strncpy(out, base, FF_MAX_LFN - 1);
        out[FF_MAX_LFN - 1] = '\0';
        return;
    }

    if (path[0] == '/') {
        snprintf(resolved, sizeof(resolved), "%s", path);
    } else {
        if (strcmp(base, "/") == 0)
            snprintf(resolved, sizeof(resolved), "/%s", path);
        else
            snprintf(resolved, sizeof(resolved), "%s/%s", base, path);
    }

    vm_device_resolve_path(resolved, out, out);
    if (out[0] == '\0') strcpy(out, "/");
}

static void vm_device_decode_drive_path(const char *input, int *fs_out, const char **path_out) {
    int fs = FatFSFileSystem;
    const char *path = input;

    if (input && input[0] && input[1] == ':') {
        if (input[0] == 'A' || input[0] == 'a') fs = 0;
        else if (input[0] == 'B' || input[0] == 'b') fs = 1;
        else error("Invalid disk");
        path = input + 2;
    }

    *fs_out = fs;
    *path_out = path;
}

static void vm_device_build_open_path(int fs, const char *input, char *out) {
    const char *path;
    char resolved[FF_MAX_LFN];

    vm_device_decode_drive_path(input, &fs, &path);
    if (path[0] == '\0') {
        strncpy(out, fs ? "0:/" : "/", FF_MAX_LFN - 1);
        out[FF_MAX_LFN - 1] = '\0';
        return;
    }

    vm_device_fullpath(fs, path, resolved);
    if (fs) snprintf(out, FF_MAX_LFN, "0:%s", resolved);
    else snprintf(out, FF_MAX_LFN, "%s", resolved);
}

static void vm_device_build_dir_path(int fs, const char *input, char *out) {
    const char *path;
    vm_device_decode_drive_path(input, &fs, &path);
    if (path[0] == '\0') {
        strncpy(out, "/", FF_MAX_LFN - 1);
        out[FF_MAX_LFN - 1] = '\0';
        return;
    }
    vm_device_fullpath(fs, path, out);
}

static void vm_device_mount_flash(void) {
    int err;
    if (vm_flash_ready) return;

    pico_lfs_cfg.block_count =
        (Option.FlashSize - RoundUpK4(TOP_OF_SYSTEM_FLASH) -
         (Option.modbuff ? 1024 * Option.modbuffsize : 0)) /
        VM_FLASH_BLOCK_SIZE;

    err = lfs_mount(&lfs, &pico_lfs_cfg);
    if (err) {
        err = lfs_format(&lfs, &pico_lfs_cfg);
        if (err) vm_device_file_error("Flash filesystem format failed", err);
        err = lfs_mount(&lfs, &pico_lfs_cfg);
        if (err) vm_device_file_error("Flash filesystem mount failed", err);
    }
    vm_flash_ready = true;
}

void vm_device_storage_init(void) {
    vm_device_mount_flash();
    vm_device_reset_paths();
    FatFSFileSystem = 0;
    FatFSFileSystemSave = 0;
    vm_loaded_program_name[0] = '\0';
}

int InitSDCard(void) {
    int rc;
    if (!FatFSFileSystem) return 1;
    if (vm_sd_ready && !(SDCardStat & STA_NOINIT)) return 1;
    rc = f_mount(&vm_fatfs, "", 1);
    if (rc) {
        FSerror = rc;
        return 0;
    }
    vm_sd_ready = true;
    return 1;
}

static void vm_device_append_bas_extension(char *filename) {
    if (strchr(filename, '.') == NULL) {
        size_t len = strlen(filename);
        if (len + 4 >= FF_MAX_LFN) error("RUN command line too long");
        strcat(filename, ".bas");
    }
}

int FileLoadSourceProgramVM(unsigned char *fname, char **source_out) {
    char filename[FF_MAX_LFN];
    char open_path[FF_MAX_LFN];
    char *buf;
    char *p;
    int fs;
    int fsize;

    if (source_out == NULL) error("Internal error");
    *source_out = NULL;

    vm_device_mount_flash();

    if (strlen((char *)fname) >= sizeof(filename))
        error("RUN command line too long");
    strcpy(filename, (char *)fname);
    vm_device_append_bas_extension(filename);

    vm_device_decode_drive_path(filename, &fs, (const char **)&fname);
    vm_device_build_open_path(fs, filename, open_path);

    if (fs) {
        FIL file;
        UINT br = 0;
        unsigned char rbuf[256];

        FatFSFileSystem = FatFSFileSystemSave = 1;
        if (!InitSDCard()) error("SD Card not found");
        FSerror = f_open(&file, open_path, FA_READ);
        if (FSerror) vm_device_file_error("File error", FSerror);
        fsize = (int)f_size(&file);
        if (fsize < 0 || fsize >= EDIT_BUFFER_SIZE - 2048 - 512) {
            f_close(&file);
            error("Not enough memory");
        }

        p = buf = (char *)bc_compile_alloc((size_t)fsize + 1u);
        if (buf == NULL) {
            f_close(&file);
            error("Not enough memory");
        }

        while (1) {
            FSerror = f_read(&file, rbuf, sizeof(rbuf), &br);
            if (FSerror) {
                f_close(&file);
                vm_device_file_error("File error", FSerror);
            }
            if (br == 0) break;
            for (UINT i = 0; i < br; ++i) {
                int c = rbuf[i] & 0x7f;
                if ((p - buf) >= fsize) {
                    f_close(&file);
                    error("Not enough memory");
                }
                if (isprint(c) || c == '\r' || c == '\n' || c == TAB) {
                    if (c == TAB) c = ' ';
                    *p++ = (char)c;
                }
            }
        }
        FSerror = f_close(&file);
        if (FSerror) vm_device_file_error("File error", FSerror);
    } else {
        lfs_file_t file;
        unsigned char rbuf[256];
        int rc;

        FatFSFileSystem = FatFSFileSystemSave = 0;
        FSerror = lfs_file_open(&lfs, &file, open_path, LFS_O_RDONLY);
        if (FSerror < 0) vm_device_file_error("File error", FSerror);
        fsize = lfs_file_size(&lfs, &file);
        if (fsize < 0 || fsize >= EDIT_BUFFER_SIZE - 2048 - 512) {
            lfs_file_close(&lfs, &file);
            error("Not enough memory");
        }

        p = buf = (char *)bc_compile_alloc((size_t)fsize + 1u);
        if (buf == NULL) {
            lfs_file_close(&lfs, &file);
            error("Not enough memory");
        }

        while ((rc = lfs_file_read(&lfs, &file, rbuf, sizeof(rbuf))) > 0) {
            for (int i = 0; i < rc; ++i) {
                int c = rbuf[i] & 0x7f;
                if ((p - buf) >= fsize) {
                    lfs_file_close(&lfs, &file);
                    error("Not enough memory");
                }
                if (isprint(c) || c == '\r' || c == '\n' || c == TAB) {
                    if (c == TAB) c = ' ';
                    *p++ = (char)c;
                }
            }
        }
        if (rc < 0) vm_device_file_error("File error", rc);
        FSerror = lfs_file_close(&lfs, &file);
        if (FSerror < 0) vm_device_file_error("File error", FSerror);
    }

    *p = 0;
    ClearSavedVars();
    *source_out = buf;
    return true;
}

static void vm_device_print_file_name(const char *name, int size) {
    char line[FF_MAX_LFN + 32];
    if (size >= 0) snprintf(line, sizeof(line), "%-28s %d\r\n", name, size);
    else snprintf(line, sizeof(line), "%s/\r\n", name);
    MMPrintString(line);
}

void vm_device_print_files(const char *spec) {
    int fs;
    char path[FF_MAX_LFN];
    char header[FF_MAX_LFN + 8];

    vm_device_mount_flash();
    vm_device_decode_drive_path(spec && *spec ? spec : "", &fs, (const char **)&spec);
    vm_device_build_dir_path(fs, spec && *spec ? spec : "", path);
    snprintf(header, sizeof(header), "%c:%s\r\n", fs ? 'B' : 'A', path);
    MMPrintString(header);

    if (fs) {
        DIR dir;
        FILINFO info;

        FatFSFileSystem = FatFSFileSystemSave = 1;
        if (!InitSDCard()) error("SD Card not found");
        char open_path[FF_MAX_LFN];
        if (strcmp(path, "/") == 0) snprintf(open_path, sizeof(open_path), "0:/");
        else snprintf(open_path, sizeof(open_path), "0:%s", path);
        FSerror = f_opendir(&dir, open_path);
        if (FSerror) vm_device_file_error("File error", FSerror);
        while (1) {
            FSerror = f_readdir(&dir, &info);
            if (FSerror) {
                f_closedir(&dir);
                vm_device_file_error("File error", FSerror);
            }
            if (info.fname[0] == '\0') break;
            if (strcmp(info.fname, ".") == 0 || strcmp(info.fname, "..") == 0) continue;
            vm_device_print_file_name(info.fname, (info.fattrib & AM_DIR) ? -1 : (int)info.fsize);
        }
        f_closedir(&dir);
    } else {
        lfs_dir_t dir;
        struct lfs_info info;

        FatFSFileSystem = FatFSFileSystemSave = 0;
        FSerror = lfs_dir_open(&lfs, &dir, path);
        if (FSerror < 0) vm_device_file_error("File error", FSerror);
        while (1) {
            FSerror = lfs_dir_read(&lfs, &dir, &info);
            if (FSerror < 0) {
                lfs_dir_close(&lfs, &dir);
                vm_device_file_error("File error", FSerror);
            }
            if (FSerror == 0) break;
            if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) continue;
            vm_device_print_file_name(info.name, info.type == LFS_TYPE_DIR ? -1 : (int)info.size);
        }
        lfs_dir_close(&lfs, &dir);
    }
}

void vm_device_print_cwd(void) {
    char buf[FF_MAX_LFN + 8];
    snprintf(buf, sizeof(buf), "%c:%s\r\n", FatFSFileSystem ? 'B' : 'A', filepath[FatFSFileSystem]);
    MMPrintString(buf);
}

bool vm_device_set_drive_spec(const char *spec) {
    int old_fs = FatFSFileSystem;
    int old_save = FatFSFileSystemSave;
    if (spec == NULL || spec[0] == '\0') return false;
    if ((spec[0] == 'A' || spec[0] == 'a') && spec[1] == ':') {
        FatFSFileSystem = FatFSFileSystemSave = 0;
        return true;
    }
    if ((spec[0] == 'B' || spec[0] == 'b') && spec[1] == ':') {
        FatFSFileSystem = FatFSFileSystemSave = 1;
        if (!InitSDCard()) {
            FatFSFileSystem = old_fs;
            FatFSFileSystemSave = old_save;
            error("SD Card not found");
        }
        return true;
    }
    return false;
}

bool vm_device_run_program(const char *spec) {
    char *source = NULL;
    const char *source_name = spec;

    if (spec == NULL || spec[0] == '\0') {
        if (vm_loaded_program_name[0] == '\0') error("No program loaded");
        source_name = vm_loaded_program_name;
    } else {
        char loaded_name[FF_MAX_LFN];
        vm_device_scan_source_file(spec, 0, NULL, NULL);
        vm_device_canonical_program_spec(spec, loaded_name);
        vm_device_program_set(loaded_name);
        source_name = vm_loaded_program_name;
    }

    bc_alloc_reset();
    FileLoadSourceProgramVM((unsigned char *)source_name, &source);
    bc_run_source_string(source, source_name);
    return true;
}

bool vm_device_program_is_loaded(void) {
    return vm_loaded_program_name[0] != '\0';
}

const char *vm_device_program_name(void) {
    return vm_loaded_program_name;
}

void vm_device_new_program(void) {
    vm_loaded_program_name[0] = '\0';
}

bool vm_device_load_program(const char *spec) {
    char loaded_name[FF_MAX_LFN];

    if (spec == NULL || spec[0] == '\0') return false;
    vm_device_scan_source_file(spec, 0, NULL, NULL);
    vm_device_canonical_program_spec(spec, loaded_name);
    vm_device_program_set(loaded_name);
    return true;
}

bool vm_device_save_program(const char *spec) {
    const char *target = spec;

    if (vm_loaded_program_name[0] == '\0') error("No program loaded");
    if (target == NULL || target[0] == '\0') {
        if (vm_loaded_program_name[0] == '\0') error("File name");
        target = vm_loaded_program_name;
    }
    vm_sys_file_copy(vm_loaded_program_name, target, 0);
    if (target != vm_loaded_program_name) {
        strncpy(vm_loaded_program_name, target, sizeof(vm_loaded_program_name) - 1);
        vm_loaded_program_name[sizeof(vm_loaded_program_name) - 1] = '\0';
    }
    return true;
}

void vm_device_list_program(const char *spec, bool all) {
    (void)all;
    if (spec && *spec) {
        vm_device_scan_source_file(spec, 1, NULL, NULL);
        return;
    }
    if (vm_loaded_program_name[0] == '\0') error("No program loaded");
    vm_device_scan_source_file(vm_loaded_program_name, 1, NULL, NULL);
}

void vm_device_print_memory_report(void) {
    char buf[128];
    size_t program_size = 0;
    int line_count = 0;
    int arena_capacity_k = (int)((bc_alloc_bytes_capacity() + 512u) / 1024u);
    int arena_used_k = (int)((bc_alloc_bytes_used() + 512u) / 1024u);
    int arena_high_k = (int)((bc_alloc_bytes_high_water() + 512u) / 1024u);
    int arena_free_k = arena_capacity_k > arena_used_k ? arena_capacity_k - arena_used_k : 0;
    int used_pct = bc_alloc_bytes_capacity() ? (int)((bc_alloc_bytes_used() * 100u) / bc_alloc_bytes_capacity()) : 0;
    int free_pct = 100 - used_pct;
    int program_size_k;

    if (vm_loaded_program_name[0] != '\0') {
        vm_device_scan_source_file(vm_loaded_program_name, 0, &program_size, &line_count);
    }
    program_size_k = (int)((program_size + 512u) / 1024u);

    MMPrintString("Program:\r\n");
    snprintf(buf, sizeof(buf), "%4dK (%2d%%) Program (%d lines)\r\n",
             program_size_k, 0, vm_loaded_program_name[0] ? line_count : 0);
    MMPrintString(buf);
    snprintf(buf, sizeof(buf), "%4dK (%2d%%) Free\r\n", 0, 100);
    MMPrintString(buf);

    MMPrintString("\r\nSaved Variables:\r\n");
    snprintf(buf, sizeof(buf), "%4dK (%2d%%) Free\r\n", 0, 100);
    MMPrintString(buf);

    MMPrintString("\r\nRAM:\r\n");
    snprintf(buf, sizeof(buf), "%4dK (%2d%%) General\r\n", arena_used_k, used_pct);
    MMPrintString(buf);
    snprintf(buf, sizeof(buf), "%4dK (%2d%%) Free\r\n", arena_free_k, free_pct);
    MMPrintString(buf);

    MMPrintString("\r\nVM arena:\r\n");
    snprintf(buf, sizeof(buf), "%4dK Capacity\r\n", arena_capacity_k);
    MMPrintString(buf);
    snprintf(buf, sizeof(buf), "%4dK Used\r\n", arena_used_k);
    MMPrintString(buf);
    snprintf(buf, sizeof(buf), "%4dK High water\r\n", arena_high_k);
    MMPrintString(buf);
}

void vm_device_help(const char *topic) {
    const char *needle = topic;
    lfs_file_t file;
    char line[STRINGSIZE];
    int rc;
    int matched = 0;
    int pos = 0;

    if (needle == NULL || *needle == '\0') {
        MMPrintString("Enter HELP and the name of the command or function\r\n");
        MMPrintString("Use * for multicharacter wildcard or ? for single character wildcard\r\n");
        return;
    }

    vm_device_mount_flash();
    rc = lfs_file_open(&lfs, &file, "/help.txt", LFS_O_RDONLY);
    if (rc < 0) error("A:/help.txt not found");

    while (*needle == ' ') needle++;
    while ((rc = lfs_file_read(&lfs, &file, &line[pos], 1)) == 1) {
        if (line[pos] == '\r') continue;
        if (line[pos] == '\n') {
            line[pos] = '\0';
            if (line[0] == '~') {
                if (matched) break;
                if (pattern_matching((char *)needle, &line[1], 0, 0)) matched = 1;
            } else if (matched) {
                MMPrintString(line);
                MMPrintString("\r\n");
            }
            pos = 0;
            continue;
        }
        if (pos + 1 < (int)sizeof(line)) pos++;
    }
    if (matched && pos > 0) {
        line[pos] = '\0';
        MMPrintString(line);
        MMPrintString("\r\n");
    }
    lfs_file_close(&lfs, &file);
}

#endif
