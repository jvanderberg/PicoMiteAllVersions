#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "vm_sys_file.h"
#include "bc_alloc.h"
#include <string.h>

static void vm_file_line_append(uint8_t *dest, int *len, int ch) {
    if (ch == '\t') {
        do {
            if (++(*len) > MAXSTRLEN) error("Line is too long");
            dest[*len] = ' ';
        } while (*len % 4);
        return;
    }
    if (++(*len) > MAXSTRLEN) error("Line is too long");
    dest[*len] = (uint8_t)ch;
}

#ifdef MMBASIC_HOST
#include "vm_host_fat.h"

static FIL *vm_files[MAXOPENFILES + 1];

static void vm_file_check_number(int fnbr) {
    if (fnbr < 1 || fnbr > MAXOPENFILES) error("File number");
}

void vm_sys_file_open(const char *filename, int fnbr, int mode) {
    BYTE fmode = 0;
    FRESULT res;

    vm_file_check_number(fnbr);
    if (vm_files[fnbr]) error("File number already open");

    switch (mode) {
        case VM_FILE_MODE_INPUT:  fmode = FA_READ; break;
        case VM_FILE_MODE_OUTPUT: fmode = FA_WRITE | FA_CREATE_ALWAYS; break;
        case VM_FILE_MODE_APPEND: fmode = FA_WRITE | FA_OPEN_APPEND; break;
        default: error("File access mode");
    }

    res = vm_host_fat_mount();
    if (res != FR_OK) error("Host FAT init failed");

    vm_files[fnbr] = (FIL *)BC_ALLOC(sizeof(FIL));
    if (!vm_files[fnbr]) error("Not enough memory");
    res = f_open(vm_files[fnbr], vm_host_fat_path(filename), fmode);
    if (res != FR_OK) {
        BC_FREE(vm_files[fnbr]);
        vm_files[fnbr] = NULL;
        error("File error");
    }
}

void vm_sys_file_close(int fnbr) {
    vm_file_check_number(fnbr);
    if (!vm_files[fnbr]) error("File number is not open");
    FRESULT res = f_close(vm_files[fnbr]);
    BC_FREE(vm_files[fnbr]);
    vm_files[fnbr] = NULL;
    if (res != FR_OK) error("File error");
}

void vm_sys_file_reset(void) {
    for (int i = 1; i <= MAXOPENFILES; i++) {
        if (vm_files[i]) {
            f_close(vm_files[i]);
            BC_FREE(vm_files[i]);
            vm_files[i] = NULL;
        }
    }
}

void vm_sys_file_print_buf(int fnbr, const char *buf, int len) {
    UINT wrote = 0;
    vm_file_check_number(fnbr);
    if (!vm_files[fnbr]) error("File number is not open");
    if (len <= 0) return;
    if (f_write(vm_files[fnbr], buf, (UINT)len, &wrote) != FR_OK ||
        wrote != (UINT)len) {
        error("File error");
    }
}

void vm_sys_file_print_str(int fnbr, const uint8_t *mstr) {
    vm_sys_file_print_buf(fnbr, (const char *)mstr + 1, mstr[0]);
}

void vm_sys_file_print_newline(int fnbr) {
    vm_sys_file_print_buf(fnbr, "\r\n", 2);
}

void vm_sys_file_line_input(int fnbr, uint8_t *dest) {
    int len = 0;
    int ch;
    vm_file_check_number(fnbr);
    if (!vm_files[fnbr]) error("File number is not open");

    while (len < MAXSTRLEN && !f_eof(vm_files[fnbr])) {
        unsigned char c = 0;
        UINT read = 0;
        if (f_read(vm_files[fnbr], &c, 1, &read) != FR_OK || read == 0) break;
        ch = c;
        if (ch == '\r') continue;
        if (ch == '\n') break;
        vm_file_line_append(dest, &len, ch);
    }
    dest[0] = (uint8_t)len;
}

void vm_sys_file_files(void) {
}

#else

typedef struct {
    unsigned char source;
    unsigned char mode;
    FIL *fat;
    lfs_file_t *flash;
} VMFileHandle;

static VMFileHandle vm_files[MAXOPENFILES + 1];

extern int FatFSFileSystem;
extern int FatFSFileSystemSave;
extern char filepath[2][FF_MAX_LFN];
extern int FSerror;

static void vm_file_check_number(int fnbr) {
    if (fnbr < 1 || fnbr > MAXOPENFILES) error("File number");
}

static void vm_file_error(int err, const char *msg) {
    FSerror = err;
    error((char *)msg);
}

static int vm_file_current_fs(void) {
    return FatFSFileSystemSave ? FatFSFileSystemSave : FatFSFileSystem;
}

static void vm_file_resolve_path(const char *filename, int *fs_out, char *path) {
    int fs = vm_file_current_fs();
    const char *name = filename;
    const char *base;
    size_t len;

    if ((filename[0] == 'A' || filename[0] == 'a' ||
         filename[0] == 'B' || filename[0] == 'b') && filename[1] == ':') {
        fs = (filename[0] == 'B' || filename[0] == 'b') ? 1 : 0;
        name = filename + 2;
    }
    if (*name == '\0') error("File name");

    if (*name == '/') {
        strncpy(path, name, FF_MAX_LFN - 1);
        path[FF_MAX_LFN - 1] = '\0';
        *fs_out = fs;
        return;
    }

    base = filepath[fs];
    if ((base[0] == 'A' || base[0] == 'B') && base[1] == ':')
        base += 2;
    if (*base == '\0') base = "/";

    len = strlen(base);
    strncpy(path, base, FF_MAX_LFN - 1);
    path[FF_MAX_LFN - 1] = '\0';
    if (len > 0 && path[strlen(path) - 1] != '/')
        strncat(path, "/", FF_MAX_LFN - strlen(path) - 1);
    strncat(path, name, FF_MAX_LFN - strlen(path) - 1);
    *fs_out = fs;
}

static int vm_file_lfs_mode(int mode) {
    switch (mode) {
        case VM_FILE_MODE_INPUT:
            return LFS_O_RDONLY;
        case VM_FILE_MODE_OUTPUT:
            return LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC;
        case VM_FILE_MODE_APPEND:
            return LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND;
        default:
            error("File access mode");
            return 0;
    }
}

static int vm_file_fat_mode(int mode) {
    switch (mode) {
        case VM_FILE_MODE_INPUT:
            return FA_READ;
        case VM_FILE_MODE_OUTPUT:
            return FA_WRITE | FA_CREATE_ALWAYS;
        case VM_FILE_MODE_APPEND:
            return FA_WRITE | FA_OPEN_APPEND;
        default:
            error("File access mode");
            return 0;
    }
}

void vm_sys_file_open(const char *filename, int fnbr, int mode) {
    char path[FF_MAX_LFN] = {0};
    int fs;

    vm_file_check_number(fnbr);
    if (vm_files[fnbr].source != NONEFILE) error("File number already open");
    vm_file_resolve_path(filename, &fs, path);

    if (fs) {
        if (!InitSDCard()) error("SD Card not found");
        vm_files[fnbr].fat = (FIL *)BC_ALLOC(sizeof(FIL));
        if (!vm_files[fnbr].fat) error("Not enough memory");
        FSerror = f_open(vm_files[fnbr].fat, path, (BYTE)vm_file_fat_mode(mode));
        if (FSerror) {
            BC_FREE(vm_files[fnbr].fat);
            memset(&vm_files[fnbr], 0, sizeof(vm_files[fnbr]));
            vm_file_error(FSerror, "File error");
        }
        vm_files[fnbr].source = FATFSFILE;
    } else {
        vm_files[fnbr].flash = (lfs_file_t *)BC_ALLOC(sizeof(lfs_file_t));
        if (!vm_files[fnbr].flash) error("Not enough memory");
        FSerror = lfs_file_open(&lfs, vm_files[fnbr].flash, path, vm_file_lfs_mode(mode));
        if (FSerror < 0) {
            BC_FREE(vm_files[fnbr].flash);
            memset(&vm_files[fnbr], 0, sizeof(vm_files[fnbr]));
            vm_file_error(FSerror, "File error");
        }
        if (mode == VM_FILE_MODE_APPEND)
            lfs_file_seek(&lfs, vm_files[fnbr].flash,
                          lfs_file_size(&lfs, vm_files[fnbr].flash),
                          LFS_SEEK_SET);
        vm_files[fnbr].source = FLASHFILE;
    }
    vm_files[fnbr].mode = (unsigned char)mode;
}

void vm_sys_file_close(int fnbr) {
    vm_file_check_number(fnbr);
    if (vm_files[fnbr].source == NONEFILE) error("File number is not open");
    if (vm_files[fnbr].source == FATFSFILE) {
        FSerror = f_close(vm_files[fnbr].fat);
        BC_FREE(vm_files[fnbr].fat);
        if (FSerror) {
            memset(&vm_files[fnbr], 0, sizeof(vm_files[fnbr]));
            vm_file_error(FSerror, "File error");
        }
    } else {
        FSerror = lfs_file_close(&lfs, vm_files[fnbr].flash);
        BC_FREE(vm_files[fnbr].flash);
        if (FSerror < 0) {
            memset(&vm_files[fnbr], 0, sizeof(vm_files[fnbr]));
            vm_file_error(FSerror, "File error");
        }
    }
    memset(&vm_files[fnbr], 0, sizeof(vm_files[fnbr]));
}

void vm_sys_file_reset(void) {
    for (int i = 1; i <= MAXOPENFILES; i++) {
        if (vm_files[i].source != NONEFILE) {
            if (vm_files[i].source == FATFSFILE) {
                f_close(vm_files[i].fat);
                BC_FREE(vm_files[i].fat);
            } else {
                lfs_file_close(&lfs, vm_files[i].flash);
                BC_FREE(vm_files[i].flash);
            }
            memset(&vm_files[i], 0, sizeof(vm_files[i]));
        }
    }
}

void vm_sys_file_print_buf(int fnbr, const char *buf, int len) {
    unsigned int wrote = 0;
    int rc;

    vm_file_check_number(fnbr);
    if (vm_files[fnbr].source == NONEFILE) error("File number is not open");
    if (len <= 0) return;

    if (vm_files[fnbr].source == FATFSFILE) {
        if (!InitSDCard()) error("SD Card not found");
        FSerror = f_write(vm_files[fnbr].fat, buf, (UINT)len, &wrote);
        if (FSerror || wrote != (unsigned int)len) vm_file_error(FSerror, "File error");
        diskchecktimer = DISKCHECKRATE;
    } else {
        rc = lfs_file_write(&lfs, vm_files[fnbr].flash, buf, len);
        if (rc != len) vm_file_error(rc < 0 ? rc : -5, "File error");
    }
}

void vm_sys_file_print_str(int fnbr, const uint8_t *mstr) {
    vm_sys_file_print_buf(fnbr, (const char *)mstr + 1, mstr[0]);
}

void vm_sys_file_print_newline(int fnbr) {
    vm_sys_file_print_buf(fnbr, "\r\n", 2);
}

static int vm_sys_file_eof(int fnbr) {
    if (vm_files[fnbr].source == FATFSFILE)
        return f_eof(vm_files[fnbr].fat);
    return lfs_file_tell(&lfs, vm_files[fnbr].flash) >=
           lfs_file_size(&lfs, vm_files[fnbr].flash);
}

static int vm_sys_file_getc(int fnbr) {
    char ch = 0;
    unsigned int read = 0;
    int rc;

    if (vm_files[fnbr].source == FATFSFILE) {
        if (!InitSDCard()) error("SD Card not found");
        FSerror = f_read(vm_files[fnbr].fat, &ch, 1, &read);
        if (FSerror) vm_file_error(FSerror, "File error");
        diskchecktimer = DISKCHECKRATE;
        return read == 1 ? (unsigned char)ch : -1;
    }

    rc = lfs_file_read(&lfs, vm_files[fnbr].flash, &ch, 1);
    if (rc < 0) vm_file_error(rc, "File error");
    return rc == 1 ? (unsigned char)ch : -1;
}

void vm_sys_file_line_input(int fnbr, uint8_t *dest) {
    int len = 0;
    int ch;
    vm_file_check_number(fnbr);
    if (vm_files[fnbr].source == NONEFILE) error("File number is not open");
    while (len < MAXSTRLEN && !vm_sys_file_eof(fnbr)) {
        ch = vm_sys_file_getc(fnbr);
        if (ch < 0) break;
        if (ch == '\r') continue;
        if (ch == '\n') break;
        vm_file_line_append(dest, &len, ch);
    }
    dest[0] = (uint8_t)len;
}

void vm_sys_file_files(void) {
}

#endif
