/*
 * host_main.c -- Host test driver for MMBasic interpreter + bytecode VM
 *
 * Loads a .bas file, runs it through the untouched legacy interpreter oracle
 * and the VM-owned raw-source frontend/bytecode VM, captures output from each,
 * and compares the results.
 *
 * Usage:
 *   ./mmbasic_test program.bas          Compare interpreter oracle vs source VM
 *   ./mmbasic_test program.bas --interp Run interpreter oracle only
 *   ./mmbasic_test program.bas --vm     Run raw-source VM only
 *   ./mmbasic_test program.bas --vm-source
 *                                      Alias for --vm
 *   ./mmbasic_test program.bas --source-compare
 *                                      Alias for default compare
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "bytecode.h"
#include "vm_sys_pin.h"
#include "vm_sys_file.h"
#include "vm_host_fat.h"

/* All needed externs come from Hardware_Includes.h / MMBasic.h */
extern char MMErrMsg[];
extern int MMerrno;
void host_runtime_configure(int timeout_ms, const char *screenshot_path);
void host_runtime_configure_keys(const char *keys, int delay_ms);
void host_runtime_begin(void);
void host_runtime_finish(void);
int host_runtime_timed_out(void);
uint32_t host_runtime_get_pixel(int x, int y);
int host_runtime_width(void);
int host_runtime_height(void);

/* flash_progmemory is NULL on host -- we need to allocate backing storage */
extern const uint8_t *flash_progmemory;
static uint8_t flash_prog_buf[256 * 1024];

/* Output capture buffer */
#define CAPTURE_SIZE (64 * 1024)
static char interp_output[CAPTURE_SIZE];
static char vm_output[CAPTURE_SIZE];
static char *capture_buf = NULL;
static char *capture_ptr = NULL;
static int capture_remaining = 0;

/* Hook into MMPrintString to capture output */
static int capturing = 0;

typedef struct {
    int x;
    int y;
    uint32_t rgb;
} PixelAssert;

typedef struct {
    int width;
    int height;
    uint32_t *pixels;
} FramebufferSnapshot;

#define MAX_PIXEL_ASSERTS 64

/* We override MMPrintString etc in host_stubs.c, so we need a way
 * to redirect output.  We'll use a global function pointer. */
void (*host_output_hook)(const char *text, int len) = NULL;

void host_capture_hook(const char *text, int len) {
    if (len > capture_remaining) len = capture_remaining;
    if (len > 0) {
        memcpy(capture_ptr, text, len);
        capture_ptr += len;
        capture_remaining -= len;
    }
}

static void strip_ansi_sequences(char *buf) {
    char *src = buf;
    char *dst = buf;

    while (*src) {
        if ((unsigned char)src[0] == 0x1B && src[1] == '[') {
            src += 2;
            while (*src && ((*src >= '0' && *src <= '9') || *src == ';' || *src == '?')) {
                src++;
            }
            if (*src) src++;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

static void start_capture(char *buf, int size) {
    capture_buf = buf;
    capture_ptr = buf;
    capture_remaining = size - 1;  /* leave room for null terminator */
    buf[0] = '\0';
    host_output_hook = host_capture_hook;
    capturing = 1;
}

static void stop_capture(void) {
    if (capture_ptr) *capture_ptr = '\0';
    host_output_hook = NULL;
    capturing = 0;
    if (capture_buf) strip_ansi_sequences(capture_buf);
}

static int parse_pixel_assert(const char *spec, PixelAssert *out) {
    char *end;
    long x = strtol(spec, &end, 10);
    if (end == spec || *end != ',') return -1;
    long y = strtol(end + 1, &end, 10);
    if (end == spec || *end != ',') return -1;
    const char *rgb_text = end + 1;
    if (*rgb_text == '#') rgb_text++;
    else if (rgb_text[0] == '0' && (rgb_text[1] == 'x' || rgb_text[1] == 'X')) rgb_text += 2;
    unsigned long rgb = strtoul(rgb_text, &end, 16);
    if (*rgb_text == '\0' || *end != '\0' || rgb > 0xFFFFFFUL) return -1;
    out->x = (int)x;
    out->y = (int)y;
    out->rgb = (uint32_t)rgb;
    return 0;
}

static int check_pixel_assertions(const char *label, const PixelAssert *asserts, int count) {
    int failures = 0;
    int width = host_runtime_width();
    int height = host_runtime_height();

    for (int i = 0; i < count; ++i) {
        const PixelAssert *pa = &asserts[i];
        if (pa->x < 0 || pa->y < 0 || pa->x >= width || pa->y >= height) {
            printf("%s pixel assertion %d out of bounds: (%d,%d) not in %dx%d\n",
                   label, i + 1, pa->x, pa->y, width, height);
            failures++;
            continue;
        }
        uint32_t actual = host_runtime_get_pixel(pa->x, pa->y);
        if (actual != pa->rgb) {
            printf("%s pixel assertion %d failed at (%d,%d): expected 0x%06X got 0x%06X\n",
                   label, i + 1, pa->x, pa->y,
                   (unsigned)pa->rgb, (unsigned)actual);
            failures++;
        }
    }

    return failures;
}

static int capture_framebuffer(FramebufferSnapshot *snap) {
    if (!snap) return -1;
    snap->width = host_runtime_width();
    snap->height = host_runtime_height();
    snap->pixels = NULL;

    if (snap->width <= 0 || snap->height <= 0) return -1;

    size_t count = (size_t)snap->width * (size_t)snap->height;
    snap->pixels = malloc(count * sizeof(*snap->pixels));
    if (!snap->pixels) return -1;

    for (int y = 0; y < snap->height; ++y) {
        for (int x = 0; x < snap->width; ++x) {
            snap->pixels[(size_t)y * (size_t)snap->width + (size_t)x] =
                host_runtime_get_pixel(x, y);
        }
    }
    return 0;
}

static void free_framebuffer_snapshot(FramebufferSnapshot *snap) {
    if (!snap) return;
    free(snap->pixels);
    snap->pixels = NULL;
    snap->width = 0;
    snap->height = 0;
}

static int compare_framebuffer_snapshot(const FramebufferSnapshot *expected) {
    if (!expected || !expected->pixels) {
        printf("Framebuffer compare failed: no interpreter snapshot\n");
        return 1;
    }

    int width = host_runtime_width();
    int height = host_runtime_height();
    if (width != expected->width || height != expected->height) {
        printf("Framebuffer dimensions differ: interpreter %dx%d, VM %dx%d\n",
               expected->width, expected->height, width, height);
        return 1;
    }

    int failures = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint32_t want = expected->pixels[(size_t)y * (size_t)width + (size_t)x];
            uint32_t got = host_runtime_get_pixel(x, y);
            if (want != got) {
                if (failures < 8) {
                    printf("Framebuffer mismatch at (%d,%d): interpreter 0x%06X VM 0x%06X\n",
                           x, y, (unsigned)want, (unsigned)got);
                }
                failures++;
            }
        }
    }

    if (failures) {
        printf("Framebuffer mismatches: %d\n", failures);
    }
    return failures;
}

/*
 * Load a .bas file into a NUL-terminated source buffer.
 * Caller must free the returned buffer.
 */
static char *read_basic_source_file(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", filename);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *source = malloc(fsize + 1);
    if (!source) { fclose(f); return NULL; }
    fread(source, 1, fsize, f);
    source[fsize] = '\0';
    fclose(f);
    return source;
}

static void host_update_continuation_setting(const char *line, unsigned char *continuation) {
    const char *p = line;
    if (!continuation) return;
    while (*p == ' ' || *p == '\t') p++;
    if (*p >= '0' && *p <= '9') {
        while (*p >= '0' && *p <= '9') p++;
        while (*p == ' ' || *p == '\t') p++;
    }
    if (strncasecmp(p, "OPTION CONTINUATION LINES ON", 28) == 0 ||
        strncasecmp(p, "OPTION CONTINUATION LINES ENABLE", 32) == 0) {
        *continuation = '_';
    } else if (strncasecmp(p, "OPTION CONTINUATION LINES OFF", 29) == 0 ||
               strncasecmp(p, "OPTION CONTINUATION LINES DISABLE", 33) == 0) {
        *continuation = 0;
    }
}

static int host_read_logical_line(const char **linep, char *out, size_t out_cap,
                                  int *physical_line_io, int *line_no_out,
                                  unsigned char *continuation) {
    const char *line = *linep;
    size_t out_len = 0;
    int line_no = *physical_line_io;

    if (*line == '\0') return 0;
    out[0] = '\0';

    while (*line) {
        const char *eol = strchr(line, '\n');
        int len = eol ? (int)(eol - line) : (int)strlen(line);
        if (len > 0 && line[len - 1] == '\r') len--;
        if (out_len + (size_t)len > out_cap - 1) len = (int)((out_cap - 1) - out_len);
        memcpy(out + out_len, line, (size_t)len);
        out_len += (size_t)len;
        out[out_len] = '\0';

        (*physical_line_io)++;
        line = eol ? eol + 1 : line + strlen(line);

        if (*continuation && out_len >= 2 &&
            out[out_len - 2] == ' ' && out[out_len - 1] == *continuation) {
            out_len -= 2;
            out[out_len] = '\0';
            continue;
        }
        break;
    }

    *linep = line;
    *line_no_out = line_no;
    host_update_continuation_setting(out, continuation);
    return 1;
}

/*
 * Tokenize source text into ProgMemory for the legacy interpreter path.
 * Returns 0 on success, -1 on error.
 */
static int load_basic_source(const char *source) {
    /* Tokenize line by line into ProgMemory */
    unsigned char *pm = ProgMemory;
    const char *line = source;
    int physical_line = 1;
    unsigned char continuation = Option.continuation;

    while (*line) {
        char logical[STRINGSIZE + 1];
        int lineno = 0;
        if (!host_read_logical_line(&line, logical, sizeof(logical), &physical_line, &lineno, &continuation))
            break;
        int len = (int)strlen(logical);

        /* Skip empty lines */
        if (len > 0) {
            /* Check if the line starts with a digit (user-provided line number).
             * If so, use the line as-is.  Otherwise prepend an auto-incrementing
             * line number so tokenise() generates a T_LINENBR. */
            const char *p = logical;
            while (p < logical + len && (*p == ' ' || *p == '\t')) p++;
            int has_user_lineno = (p < logical + len && *p >= '0' && *p <= '9');

            if (has_user_lineno) {
                memcpy(inpbuf, logical, len);
                inpbuf[len] = '\0';
            } else {
                char numbuf[16];
                snprintf(numbuf, sizeof(numbuf), "%d ", lineno);
                int nlen = strlen(numbuf);
                memcpy(inpbuf, numbuf, nlen);
                memcpy(inpbuf + nlen, logical, len);
                inpbuf[nlen + len] = '\0';
            }

            /* Tokenize -- tokenise(0) adds T_NEWLINE and T_LINENBR */
            tokenise(0);

            /* Copy tokenized output to ProgMemory.
             * tokenise() terminates tknbuf with two+ zero bytes.
             * T_LINENBR contains embedded single zero bytes (for line
             * numbers < 256), so we can't use while(*tp) -- instead
             * copy until we see two consecutive zero bytes (same as
             * SaveProgramToFlash in PicoMite.c:4790). */
            unsigned char *tp = tknbuf;
            while (!(tp[0] == 0 && tp[1] == 0)) {
                *pm++ = *tp++;
            }
            *pm++ = 0;  /* element terminator */
        }
    }

    /* Program terminator */
    *pm++ = 0;
    *pm++ = 0;
    PSize = (int)(pm - ProgMemory);

    return 0;
}

static int load_basic_file(const char *filename) {
    char *source = read_basic_source_file(filename);
    if (!source) return -1;
    int rc = load_basic_source(source);
    free(source);
    return rc;
}

/*
 * Run via the old interpreter with output capture.
 * Returns 0 on normal completion, non-zero on error.
 */
static int run_interpreter(char *output, int outsize) {
    start_capture(output, outsize);
    int result = 0;

    vm_host_fat_reset();
    vm_sys_file_reset();
    vm_sys_pin_reset();
    ClearRuntime(true);
    MMErrMsg[0] = '\0';
    MMerrno = 0;
    host_runtime_begin();
    PrepareProgram(1);
    if (setjmp(mark) == 0) {
        ExecuteProgram(ProgMemory);
        result = host_runtime_timed_out() ? 2 : (MMErrMsg[0] ? 1 : 0);
    } else {
        result = host_runtime_timed_out() ? 2 : (MMErrMsg[0] ? 1 : 0);
    }

    host_runtime_finish();
    stop_capture();
    return result;
}

/*
 * Run via the bytecode VM with output capture.
 * Returns 0 on normal completion, non-zero on error.
 */
static int run_bytecode_vm_source(const char *source, const char *source_name,
                                  char *output, int outsize) {
    start_capture(output, outsize);
    int result = 0;

    vm_host_fat_reset();
    vm_sys_file_reset();
    vm_sys_pin_reset();
    ClearRuntime(true);
    MMErrMsg[0] = '\0';
    MMerrno = 0;
    host_runtime_begin();
    if (setjmp(mark) == 0) {
        bc_run_source_string(source, source_name);
        result = host_runtime_timed_out() ? 2 : (MMErrMsg[0] ? 1 : 0);
    } else {
        result = host_runtime_timed_out() ? 2 : (MMErrMsg[0] ? 1 : 0);
    }

    host_runtime_finish();
    stop_capture();
    return result;
}

typedef enum {
    MODE_SOURCE_COMPARE = 0,
    MODE_INTERP_ONLY,
    MODE_VM_SOURCE_ONLY,
    MODE_IMMEDIATE,
    MODE_TRY_COMPILE
} HostMode;

int main(int argc, char **argv) {
    HostMode mode = MODE_SOURCE_COMPARE;
    int timeout_ms = 0;
    const char *screenshot_path = NULL;
    int dump_vm_disasm = 0;
    int opt_level = 1;
    const char *immediate_line = NULL;
    PixelAssert pixel_asserts[MAX_PIXEL_ASSERTS];
    int pixel_assert_count = 0;
    int compare_framebuffer = 0;

    if (argc < 2) {
        printf("MMBasic Host Test Build\n");
        printf("======================\n\n");
        printf("Usage:\n");
        printf("  %s program.bas          Compare interpreter oracle vs raw-source VM\n", argv[0]);
        printf("  %s program.bas --interp Run interpreter oracle only\n", argv[0]);
        printf("  %s program.bas --vm     Run raw-source VM only\n", argv[0]);
        printf("  %s program.bas --vm-disasm Dump VM disassembly, then run VM\n", argv[0]);
        printf("  %s program.bas --vm -O0 Disable VM frontend peephole opts\n", argv[0]);
        printf("  %s program.bas --vm -O1 Enable VM frontend peephole opts\n", argv[0]);
        printf("  %s program.bas --vm-source Alias for --vm\n", argv[0]);
        printf("  %s program.bas --source-compare Alias for default compare\n", argv[0]);
        printf("  %s program.bas --interp --timeout-ms 100 --screenshot out.ppm\n", argv[0]);
        printf("  %s program.bas --vm --assert-pixel 10,20,FF0000\n", argv[0]);
        printf("  %s --immediate \"PRINT 2+2\"   Compile+execute one BASIC line\n", argv[0]);
        printf("  %s --try-compile \"PRINT 2+2\" Test if a line compiles (exit 0=yes 1=no)\n", argv[0]);
        printf("  %s program.bas --compare-framebuffer\n", argv[0]);
        printf("  %s program.bas --interp --keys-after-ms 100 q\n", argv[0]);
        return 0;
    }

    const char *filename = NULL;
    const char *key_script = NULL;
    int key_delay_ms = 0;

    /* First pass: scan for --immediate / --try-compile (no filename needed) */
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--immediate") == 0 && i + 1 < argc) {
            mode = MODE_IMMEDIATE;
            immediate_line = argv[++i];
        } else if (strcmp(argv[i], "--try-compile") == 0 && i + 1 < argc) {
            mode = MODE_TRY_COMPILE;
            immediate_line = argv[++i];
        }
    }

    /* For non-immediate modes, first positional arg is the filename */
    if (mode != MODE_IMMEDIATE && mode != MODE_TRY_COMPILE) {
        filename = argv[1];
    }

    for (int i = (filename ? 2 : 1); i < argc; ++i) {
        if (strcmp(argv[i], "--immediate") == 0 && i + 1 < argc) { i++; continue; }
        if (strcmp(argv[i], "--try-compile") == 0 && i + 1 < argc) { i++; continue; }
        if (strcmp(argv[i], "--interp") == 0) mode = MODE_INTERP_ONLY;
        else if (strcmp(argv[i], "--vm") == 0) mode = MODE_VM_SOURCE_ONLY;
        else if (strcmp(argv[i], "--vm-disasm") == 0) {
            mode = MODE_VM_SOURCE_ONLY;
            dump_vm_disasm = 1;
        }
        else if (strcmp(argv[i], "-O0") == 0) opt_level = 0;
        else if (strcmp(argv[i], "-O1") == 0) opt_level = 1;
        else if (strcmp(argv[i], "--vm-source") == 0) mode = MODE_VM_SOURCE_ONLY;
        else if (strcmp(argv[i], "--source-compare") == 0) mode = MODE_SOURCE_COMPARE;
        else if (strcmp(argv[i], "--immediate") == 0 && i + 1 < argc) {
            mode = MODE_IMMEDIATE;
            immediate_line = argv[++i];
        }
        else if (strcmp(argv[i], "--try-compile") == 0 && i + 1 < argc) {
            mode = MODE_TRY_COMPILE;
            immediate_line = argv[++i];
        }
        else if (strcmp(argv[i], "--compare-framebuffer") == 0) compare_framebuffer = 1;
        else if (strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc) {
            timeout_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot_path = argv[++i];
        } else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
            key_script = argv[++i];
            key_delay_ms = 0;
        } else if (strcmp(argv[i], "--keys-after-ms") == 0 && i + 2 < argc) {
            key_delay_ms = atoi(argv[++i]);
            key_script = argv[++i];
        } else if (strcmp(argv[i], "--assert-pixel") == 0 && i + 1 < argc) {
            if (pixel_assert_count >= MAX_PIXEL_ASSERTS) {
                fprintf(stderr, "Too many --assert-pixel options (max %d)\n", MAX_PIXEL_ASSERTS);
                return 2;
            }
            if (parse_pixel_assert(argv[++i], &pixel_asserts[pixel_assert_count]) != 0) {
                fprintf(stderr, "Invalid --assert-pixel spec: %s\n", argv[i]);
                return 2;
            }
            pixel_assert_count++;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    /* Allocate backing storage for flash_progmemory (normally in flash on device).
     * First half is program area (zeroed), second half mimics erased flash (0xFF)
     * so PrepareProgramExt finds the CFunction terminator. */
    memset(flash_prog_buf, 0, sizeof(flash_prog_buf) / 2);
    memset(flash_prog_buf + sizeof(flash_prog_buf) / 2, 0xFF, sizeof(flash_prog_buf) / 2);
    flash_progmemory = flash_prog_buf;

    /* Initialize the MMBasic runtime */
    InitBasic();
    bc_opt_level = opt_level;
    host_runtime_configure(timeout_ms, screenshot_path);
    host_runtime_configure_keys(key_script, key_delay_ms);

    /* Immediate mode: compile+execute a single BASIC line */
    if (mode == MODE_IMMEDIATE) {
        int rc;
        start_capture(vm_output, CAPTURE_SIZE);
        vm_host_fat_reset();
        vm_sys_file_reset();
        MMErrMsg[0] = '\0';
        MMerrno = 0;
        host_runtime_begin();
        if (setjmp(mark) == 0) {
            bc_run_immediate(immediate_line);
            rc = MMErrMsg[0] ? 1 : 0;
        } else {
            rc = MMErrMsg[0] ? 1 : 0;
        }
        host_runtime_finish();
        stop_capture();
        printf("%s", vm_output);
        if (rc && MMErrMsg[0]) fprintf(stderr, "Error: %s\n", MMErrMsg);
        return rc;
    }

    /* Try-compile mode: test if a line compiles, exit 0=yes 1=no */
    if (mode == MODE_TRY_COMPILE) {
        return bc_try_compile_line(immediate_line) ? 0 : 1;
    }

    char *source_text = read_basic_source_file(filename);
    if (!source_text) return 1;

    if (mode != MODE_VM_SOURCE_ONLY) {
        if (load_basic_source(source_text) != 0) {
            free(source_text);
            return 1;
        }
    }

    if (mode == MODE_INTERP_ONLY) {
        /* Interpreter only */
        printf("--- Interpreter ---\n");
        int rc = run_interpreter(interp_output, CAPTURE_SIZE);
        printf("%s", interp_output);
        int pixel_failures = (rc == 0) ? check_pixel_assertions("Interpreter", pixel_asserts, pixel_assert_count) : 0;
        if (rc == 2) printf("\n--- Timed Out ---\n");
        else if (pixel_failures) printf("\n--- Pixel Assertions Failed ---\n");
        else printf("\n--- Done ---\n");
        free(source_text);
        if (rc == 2) return 124;
        return (rc != 0 || pixel_failures) ? 1 : 0;
    }

    if (mode == MODE_VM_SOURCE_ONLY) {
        bc_debug_enabled = dump_vm_disasm ? 1 : 0;
        printf("--- Bytecode VM Source Frontend ---\n");
        int rc = run_bytecode_vm_source(source_text, filename, vm_output, CAPTURE_SIZE);
        bc_debug_enabled = 0;
        printf("%s", vm_output);
        int pixel_failures = (rc == 0) ? check_pixel_assertions("Bytecode VM Source", pixel_asserts, pixel_assert_count) : 0;
        if (rc == 2) printf("\n--- Timed Out ---\n");
        else if (pixel_failures) printf("\n--- Pixel Assertions Failed ---\n");
        else printf("\n--- Done ---\n");
        free(source_text);
        if (rc == 2) return 124;
        return (rc != 0 || pixel_failures) ? 1 : 0;
    }

    if (mode == MODE_SOURCE_COMPARE) {
        FramebufferSnapshot interp_frame = {0, 0, NULL};
        int fb_compare_failures = 0;
        printf("Running source frontend compare: %s\n\n", filename);

        printf("--- Interpreter ---\n");
        int r1 = run_interpreter(interp_output, CAPTURE_SIZE);
        int p1 = (r1 == 0) ? check_pixel_assertions("Interpreter", pixel_asserts, pixel_assert_count) : 0;
        if (compare_framebuffer && r1 == 0) {
            if (capture_framebuffer(&interp_frame) != 0) {
                printf("Framebuffer compare failed: could not capture interpreter framebuffer\n");
                fb_compare_failures = 1;
            }
        }

        printf("--- Bytecode VM Source Frontend ---\n");
        int r2 = run_bytecode_vm_source(source_text, filename, vm_output, CAPTURE_SIZE);
        int p2 = (r2 == 0) ? check_pixel_assertions("Bytecode VM Source", pixel_asserts, pixel_assert_count) : 0;
        if (compare_framebuffer && r1 == 0 && r2 == 0 && fb_compare_failures == 0) {
            fb_compare_failures = compare_framebuffer_snapshot(&interp_frame);
        }

        printf("\n--- Results ---\n");
        printf("Interpreter: %s\n", r1 == 0 ? "OK" : (r1 == 2 ? "TIMEOUT" : "ERROR"));
        printf("Bytecode VM Source: %s\n", r2 == 0 ? "OK" : (r2 == 2 ? "TIMEOUT" : "ERROR"));
        if (pixel_assert_count > 0) {
            printf("Interpreter pixels: %s\n", p1 == 0 ? "OK" : "FAIL");
            printf("Bytecode VM Source pixels: %s\n", p2 == 0 ? "OK" : "FAIL");
        }
        if (compare_framebuffer) {
            printf("Framebuffer compare: %s\n", fb_compare_failures == 0 ? "OK" : "FAIL");
        }

        if (r1 != 0 || r2 != 0 || p1 != 0 || p2 != 0 || fb_compare_failures != 0) {
            printf("\nInterpreter output:\n---\n%s\n---\n\n", interp_output);
            printf("Bytecode VM Source output:\n---\n%s\n---\n\n", vm_output);
            free(source_text);
            free_framebuffer_snapshot(&interp_frame);
            return 1;
        }

        if (strcmp(interp_output, vm_output) == 0) {
            printf("Output: MATCH\n");
            printf("\nOutput (%d chars):\n%s\n", (int)strlen(interp_output), interp_output);
            free(source_text);
            free_framebuffer_snapshot(&interp_frame);
            return 0;
        }

        printf("Output: MISMATCH!\n\n");
        printf("Interpreter output:\n---\n%s\n---\n\n", interp_output);
        printf("Bytecode VM Source output:\n---\n%s\n---\n\n", vm_output);
        free(source_text);
        free_framebuffer_snapshot(&interp_frame);
        return 1;
    }
    free(source_text);
    return 1;
}
