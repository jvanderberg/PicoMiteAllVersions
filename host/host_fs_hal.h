/* host_fs_hal.h — host-build shunt for file-system primitives.
 *
 * FileIO.c now compiles on host (see docs/host-hal-plan.md, Phase 3).
 * On host, BasicFileOpen has two modes:
 *   1) host_sd_root set (REPL / --sim) — route through POSIX fopen() so
 *      EDIT "foo.bas" / RUN "foo.bas" / OPEN "data.csv" read and write
 *      real files under the user's chosen directory.
 *   2) host_sd_root NULL (test harness) — fall through to FileIO.c's
 *      existing FatFS path, which is backed by vm_host_fat.c's RAM disk.
 *
 * Each file primitive in FileIO.c (BasicFileOpen, FileGetChar,
 * FilePutChar, FileClose, FileEOF, fun_loc, fun_lof, cmd_seek,
 * cmd_flush) adds a small #ifdef MMBASIC_HOST preamble:
 *
 *     #ifdef MMBASIC_HOST
 *     if (host_fs_posix_active(fnbr)) return host_fs_posix_<op>(...);
 *     #endif
 *
 * If the fnbr has a POSIX side-table entry, the host helper services it
 * and returns; otherwise the regular FatFS / LFS path runs unchanged.
 *
 * The side-table is populated by host_fs_posix_try_open(), which
 * BasicFileOpen calls first. If host_sd_root is NULL or the fopen()
 * fails, it returns 0 and BasicFileOpen proceeds with its normal body. */
#ifndef HOST_FS_HAL_H
#define HOST_FS_HAL_H

#include <stdint.h>

int host_fs_posix_active(int fnbr);
int host_fs_posix_try_open(char *fname, int fnbr, int mode);
char host_fs_posix_get_char(int fnbr);
char host_fs_posix_put_char(char c, int fnbr);
void host_fs_posix_put_str(int count, char *s, int fnbr);
int host_fs_posix_read_bytes(int fnbr, void *buf, int count);
int host_fs_posix_write_bytes(int fnbr, const void *buf, int count);
int host_fs_posix_eof(int fnbr);
void host_fs_posix_close(int fnbr);
int64_t host_fs_posix_loc(int fnbr);
int64_t host_fs_posix_lof(int fnbr);
void host_fs_posix_seek(int fnbr, int64_t offset);
void host_fs_posix_flush(int fnbr);

#endif
