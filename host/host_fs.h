#ifndef HOST_FS_H
#define HOST_FS_H

/*
 * POSIX-backed filesystem helpers for the host REPL / simulator.
 * Kept isolated from MMBasic_Includes.h so POSIX's DIR / dirent / setmode
 * symbols don't collide with FatFS's and MMBasic's own declarations.
 *
 * All paths passed in are expected to be already resolved (absolute or
 * relative to cwd) — the REPL's path-resolve step lives in the stubs.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*host_fs_emit_line)(const char *name);

/* List entries of `dir`, filtering by glob `pattern`. For each match,
 * call `emit(name)`. Returns 0 on success, -1 on error. */
int host_fs_list_dir(const char *dir, const char *pattern, host_fs_emit_line emit);

#ifdef __cplusplus
}
#endif

#endif
