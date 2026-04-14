#ifdef PICOMITE_VM_DEVICE_ONLY

#include <ctype.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "bytecode.h"
#include "bc_alloc.h"
#include "vm_device_support.h"
#include "vm_device_fileio.h"
#include "vm_sys_file.h"

extern jmp_buf mark;
extern char filepath[2][FF_MAX_LFN];
void vm_device_runtime_init(void);

static void vm_main_log(const char *s) {
    while (*s) putchar_raw(*s++);
    putchar_raw('\r');
    putchar_raw('\n');
}

typedef void (*vm_shell_handler_t)(char *args);

typedef struct {
    const char *name;
    vm_shell_handler_t handler;
} vm_shell_command_t;

static void vm_device_prompt(char *buf, size_t len) {
    snprintf(buf, len, "%c:%s> ", FatFSFileSystem ? 'B' : 'A', filepath[FatFSFileSystem]);
}

static void vm_device_read_line(char *buf, size_t len) {
    size_t pos = 0;
    buf[0] = '\0';
    while (1) {
        int c = MMgetchar();
        if (c == '\r' || c == '\n') {
            MMPrintString("\r\n");
            break;
        }
        if (c == 8 || c == 127) {
            if (pos > 0) {
                pos--;
                buf[pos] = '\0';
                MMPrintString("\b \b");
            }
            continue;
        }
        if (c < 32 || c > 126) continue;
        if (pos + 1 >= len) continue;
        buf[pos++] = (char)c;
        buf[pos] = '\0';
        MMputchar((char)c, 1);
    }
}

static char *vm_device_trim(char *s) {
    char *end;
    while (*s && isspace((unsigned char)*s)) s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

static int vm_device_kw_eq(const char *s, const char *kw) {
    while (*kw && *s) {
        if (toupper((unsigned char)*s) != toupper((unsigned char)*kw)) return 0;
        s++;
        kw++;
    }
    return *kw == '\0' && (*s == '\0' || isspace((unsigned char)*s));
}

static char *vm_device_unquote(char *s) {
    size_t n;
    s = vm_device_trim(s);
    n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        s[n - 1] = '\0';
        s++;
    }
    return s;
}

static int vm_device_kw_starts(const char *s, const char *kw) {
    while (*kw && *s) {
        if (toupper((unsigned char)*s) != toupper((unsigned char)*kw)) return 0;
        s++;
        kw++;
    }
    return *kw == '\0' && (*s == '\0' || isspace((unsigned char)*s));
}

static char *vm_device_find_keyword(char *s, const char *kw) {
    size_t n = strlen(kw);
    char *base = s;
    while (*s) {
        if (strncasecmp(s, kw, n) == 0 &&
            (s == base || isspace((unsigned char)s[-1])) &&
            (s[n] == '\0' || isspace((unsigned char)s[n]))) {
            return s;
        }
        s++;
    }
    return NULL;
}

static void vm_device_print_help(void) {
    MMPrintString("Commands: RUN, LOAD, SAVE, LIST, NEW, FILES, DRIVE, CHDIR, MKDIR, RMDIR, COPY, RENAME, HELP, MEMORY, CLS, PWD, FREE\r\n");
}

static void vm_device_cmd_unimplemented(char *args) { (void)args; error("Command not implemented"); }
static void vm_device_cmd_help(char *args) { vm_device_help(vm_device_unquote(args)); }
static void vm_device_cmd_pwd(char *args) { (void)args; vm_device_print_cwd(); }
static void vm_device_cmd_cls(char *args) { (void)args; ClearScreen(Option.DefaultBC); }
static void vm_device_cmd_free(char *args) {
    char buf[96];
    (void)args;
    snprintf(buf, sizeof(buf), "heap %u / %u bytes\r\n",
             (unsigned)bc_alloc_bytes_used(),
             (unsigned)bc_alloc_bytes_capacity());
    MMPrintString(buf);
}
static void vm_device_cmd_memory(char *args) { (void)args; vm_device_print_memory_report(); }
static void vm_device_cmd_drive(char *args) {
    if (!vm_device_set_drive_spec(vm_device_unquote(args))) error("Syntax");
}
static void vm_device_cmd_files(char *args) { vm_device_print_files(vm_device_unquote(args)); }
static void vm_device_cmd_cd(char *args) {
    vm_sys_file_chdir(vm_device_unquote(args));
    FatFSFileSystemSave = FatFSFileSystem;
}
static void vm_device_cmd_mkdir(char *args) { vm_sys_file_mkdir(vm_device_unquote(args)); }
static void vm_device_cmd_rmdir(char *args) { vm_sys_file_rmdir(vm_device_unquote(args)); }
static void vm_device_cmd_kill(char *args) { vm_sys_file_kill(vm_device_unquote(args)); }
static void vm_device_cmd_new(char *args) { (void)args; vm_device_new_program(); }
static void vm_device_cmd_load(char *args) {
    char *comma;
    char *spec = vm_device_trim(args);
    int autorun = 0;
    if (*spec == '\0') error("Syntax");
    comma = strrchr(spec, ',');
    if (comma != NULL) {
        *comma++ = '\0';
        comma = vm_device_trim(comma);
        if (*comma) {
            if (toupper((unsigned char)*comma) == 'R' && comma[1] == '\0') autorun = 1;
            else error("Syntax");
        }
    }
    spec = vm_device_unquote(spec);
    vm_device_load_program(spec);
    if (autorun) vm_device_run_program(NULL);
}
static void vm_device_cmd_save(char *args) {
    char *spec = vm_device_unquote(args);
    if (*vm_device_trim(spec) == '\0') spec = NULL;
    vm_device_save_program(spec);
}
static void vm_device_cmd_list(char *args) {
    char *spec = vm_device_trim(args);
    if (vm_device_kw_starts(spec, "ALL")) {
        spec += 3;
        spec = vm_device_trim(spec);
        vm_device_list_program(*spec ? vm_device_unquote(spec) : NULL, true);
        return;
    }
    if (*spec == '\0') vm_device_list_program(NULL, false);
    else vm_device_list_program(vm_device_unquote(spec), false);
}
static void vm_device_cmd_run(char *args) {
    char *spec = vm_device_unquote(args);
    if (*vm_device_trim(spec) == '\0') spec = NULL;
    vm_device_run_program(spec);
}
static void vm_device_cmd_copy(char *args) {
    char *to = vm_device_find_keyword(args, "TO");
    if (to == NULL) error("Syntax");
    *to = '\0';
    to += 2;
    vm_sys_file_copy(vm_device_unquote(vm_device_trim(args)),
                     vm_device_unquote(vm_device_trim(to)), 0);
}
static void vm_device_cmd_rename(char *args) {
    char *as = vm_device_find_keyword(args, "AS");
    if (as == NULL) error("Syntax");
    *as = '\0';
    as += 2;
    vm_sys_file_rename(vm_device_unquote(vm_device_trim(args)),
                       vm_device_unquote(vm_device_trim(as)));
}

static const vm_shell_command_t vm_shell_commands[] = {
    {"CMM2 LOAD", vm_device_cmd_load},
    {"CMM2 RUN", vm_device_cmd_run},
    {"CHDIR", vm_device_cmd_cd},
    {"DRIVE", vm_device_cmd_drive},
    {"FILES", vm_device_cmd_files},
    {"MKDIR", vm_device_cmd_mkdir},
    {"RMDIR", vm_device_cmd_rmdir},
    {"RENAME", vm_device_cmd_rename},
    {"MEMORY", vm_device_cmd_memory},
    {"OPTION", vm_device_cmd_unimplemented},
    {"CONFIGURE", vm_device_cmd_unimplemented},
    {"AUTOSAVE", vm_device_cmd_unimplemented},
    {"EDIT", vm_device_cmd_unimplemented},
    {"HELP", vm_device_cmd_help},
    {"LIST", vm_device_cmd_list},
    {"LOAD", vm_device_cmd_load},
    {"SAVE", vm_device_cmd_save},
    {"COPY", vm_device_cmd_copy},
    {"KILL", vm_device_cmd_kill},
    {"FREE", vm_device_cmd_free},
    {"NEW", vm_device_cmd_new},
    {"PWD", vm_device_cmd_pwd},
    {"CLS", vm_device_cmd_cls},
    {"RUN", vm_device_cmd_run},
    {"CD", vm_device_cmd_cd},
};

static const vm_shell_command_t *vm_device_find_command(char *line, char **args_out) {
    size_t best_len = 0;
    const vm_shell_command_t *best = NULL;
    for (size_t i = 0; i < sizeof(vm_shell_commands) / sizeof(vm_shell_commands[0]); ++i) {
        const char *name = vm_shell_commands[i].name;
        size_t len = strlen(name);
        if (len < best_len) continue;
        if (vm_device_kw_eq(line, name)) {
            best = &vm_shell_commands[i];
            best_len = len;
        }
    }
    if (!best) return NULL;
    *args_out = line + best_len;
    while (**args_out && isspace((unsigned char)**args_out)) (*args_out)++;
    return best;
}

int MIPS16 main(void) {
    char line[STRINGSIZE];
    char prompt[FF_MAX_LFN + 8];

    vm_device_runtime_init();
    vm_main_log("VMMAIN 1 after_runtime");
    vm_device_storage_init();
    vm_main_log("VMMAIN 2 after_storage");
    ResetDisplay();
    vm_main_log("VMMAIN 3 after_resetdisplay");
    MMPrintString("\r\n");
    vm_main_log("VMMAIN 4 after_newline");

    while (1) {
        char *cmd;
        char *arg = NULL;
        const vm_shell_command_t *entry = NULL;

        vm_device_prompt(prompt, sizeof(prompt));
        vm_main_log("VMMAIN 5 before_prompt");
        MMPrintString(prompt);
        vm_main_log("VMMAIN 6 after_prompt");
        vm_device_read_line(line, sizeof(line));
        cmd = vm_device_trim(line);
        if (*cmd == '\0') continue;
        if (vm_device_set_drive_spec(cmd)) continue;

        if (setjmp(mark) != 0) {
            bc_compile_release_all();
            vm_sys_file_reset();
            continue;
        }

        entry = vm_device_find_command(cmd, &arg);
        if (entry) {
            entry->handler(arg);
        } else {
            cmd = vm_device_unquote(cmd);
            vm_device_run_program(cmd);
        }
    }
    return 0;
}

#endif
