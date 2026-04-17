/*
 * host_fs.c — POSIX filesystem helpers for the host REPL.
 *
 * Isolated from MMBasic_Includes.h because both FatFS (ff.h) and POSIX
 * (dirent.h) declare DIR, and POSIX unistd.h's setmode() collides with
 * MMBasic's graphics setmode(). Keeping this code here means those
 * headers never meet.
 */

#include <dirent.h>
#include <string.h>
#include <ctype.h>

#include "host_fs.h"

/* Tiny glob matcher — handles '*' and '?'. Matches semantics with
 * vm_file_match_pattern in vm_sys_file.c (case-insensitive). */
static int host_fs_match(const char *pattern, const char *name) {
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1;
            while (*name) {
                if (host_fs_match(pattern, name)) return 1;
                name++;
            }
            return 0;
        }
        if (*pattern == '?') {
            if (!*name) return 0;
            pattern++;
            name++;
            continue;
        }
        if (toupper((unsigned char)*pattern) != toupper((unsigned char)*name))
            return 0;
        pattern++;
        name++;
    }
    return *name == '\0';
}

int host_fs_list_dir(const char *dir, const char *pattern, host_fs_emit_line emit) {
    DIR *d = opendir(dir);
    if (!d) return -1;

    const char *pat = (pattern && *pattern) ? pattern : "*";
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;  /* hide dotfiles and . / .. */
        if (!host_fs_match(pat, de->d_name)) continue;
        emit(de->d_name);
    }
    closedir(d);
    return 0;
}
