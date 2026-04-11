/*
 * host_main.c -- Host test driver for MMBasic interpreter + bytecode VM
 *
 * Loads a .bas file, runs it through both the old interpreter and the new
 * bytecode VM, captures output from each, and compares the results.
 *
 * Usage:
 *   ./mmbasic_test program.bas          Compare both engines
 *   ./mmbasic_test program.bas --interp Run interpreter only
 *   ./mmbasic_test program.bas --vm     Run bytecode VM only
 *   ./mmbasic_test program.bas --debug  Compile-only: show stats + disassembly
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "Hardware_Includes.h"
#include "bytecode.h"

/* All needed externs come from Hardware_Includes.h / MMBasic.h */
extern char MMErrMsg[];
extern int MMerrno;
void host_runtime_configure(int timeout_ms, const char *screenshot_path);
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

/*
 * Load a .bas file and tokenize it into ProgMemory.
 * Returns 0 on success, -1 on error.
 */
static int load_basic_file(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", filename);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *source = malloc(fsize + 1);
    if (!source) { fclose(f); return -1; }
    fread(source, 1, fsize, f);
    source[fsize] = '\0';
    fclose(f);

    /* Tokenize line by line into ProgMemory */
    unsigned char *pm = ProgMemory;
    char *line = source;
    int lineno = 1;

    while (*line) {
        char *eol = strchr(line, '\n');
        int len = eol ? (int)(eol - line) : (int)strlen(line);

        /* Strip CR */
        if (len > 0 && line[len - 1] == '\r') len--;

        /* Skip empty lines */
        if (len > 0) {
            /* Check if the line starts with a digit (user-provided line number).
             * If so, use the line as-is.  Otherwise prepend an auto-incrementing
             * line number so tokenise() generates a T_LINENBR. */
            const char *p = line;
            while (p < line + len && (*p == ' ' || *p == '\t')) p++;
            int has_user_lineno = (p < line + len && *p >= '0' && *p <= '9');

            if (has_user_lineno) {
                memcpy(inpbuf, line, len);
                inpbuf[len] = '\0';
            } else {
                char numbuf[16];
                snprintf(numbuf, sizeof(numbuf), "%d ", lineno);
                int nlen = strlen(numbuf);
                memcpy(inpbuf, numbuf, nlen);
                memcpy(inpbuf + nlen, line, len);
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

        lineno++;
        line = eol ? eol + 1 : line + strlen(line);
    }

    /* Program terminator */
    *pm++ = 0;
    *pm++ = 0;
    PSize = (int)(pm - ProgMemory);

    free(source);
    return 0;
}

/*
 * Run via the old interpreter with output capture.
 * Returns 0 on normal completion, non-zero on error.
 */
static int run_interpreter(char *output, int outsize) {
    start_capture(output, outsize);
    int result = 0;

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
static int run_bytecode_vm(char *output, int outsize) {
    start_capture(output, outsize);
    int result = 0;

    ClearRuntime(true);
    MMErrMsg[0] = '\0';
    MMerrno = 0;
    host_runtime_begin();
    PrepareProgram(1);
    if (setjmp(mark) == 0) {
        bc_run_current_program();
        result = host_runtime_timed_out() ? 2 : (MMErrMsg[0] ? 1 : 0);
    } else {
        result = host_runtime_timed_out() ? 2 : (MMErrMsg[0] ? 1 : 0);
    }

    host_runtime_finish();
    stop_capture();
    return result;
}

int main(int argc, char **argv) {
    int mode = 0;  /* 0=compare, 1=interp only, 2=vm only, 3=debug */
    int timeout_ms = 0;
    const char *screenshot_path = NULL;
    PixelAssert pixel_asserts[MAX_PIXEL_ASSERTS];
    int pixel_assert_count = 0;

    if (argc < 2) {
        printf("MMBasic Host Test Build\n");
        printf("======================\n\n");
        printf("Usage:\n");
        printf("  %s program.bas          Compare both engines\n", argv[0]);
        printf("  %s program.bas --interp Run interpreter only\n", argv[0]);
        printf("  %s program.bas --vm     Run bytecode VM only\n", argv[0]);
        printf("  %s program.bas --debug  Compile + show stats/disassembly\n", argv[0]);
        printf("  %s program.bas --interp --timeout-ms 100 --screenshot out.ppm\n", argv[0]);
        printf("  %s program.bas --vm --assert-pixel 10,20,FF0000\n", argv[0]);
        return 0;
    }

    const char *filename = argv[1];

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--interp") == 0) mode = 1;
        else if (strcmp(argv[i], "--vm") == 0) mode = 2;
        else if (strcmp(argv[i], "--debug") == 0) mode = 3;
        else if (strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc) {
            timeout_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot_path = argv[++i];
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
    host_runtime_configure(timeout_ms, screenshot_path);

    /* Load the program */
    if (load_basic_file(filename) != 0) return 1;

    if (mode == 1) {
        /* Interpreter only */
        printf("--- Interpreter ---\n");
        int rc = run_interpreter(interp_output, CAPTURE_SIZE);
        printf("%s", interp_output);
        int pixel_failures = (rc == 0) ? check_pixel_assertions("Interpreter", pixel_asserts, pixel_assert_count) : 0;
        if (rc == 2) printf("\n--- Timed Out ---\n");
        else if (pixel_failures) printf("\n--- Pixel Assertions Failed ---\n");
        else printf("\n--- Done ---\n");
        if (rc == 2) return 124;
        return (rc != 0 || pixel_failures) ? 1 : 0;
    }

    if (mode == 2) {
        /* Bytecode VM only */
        printf("--- Bytecode VM ---\n");
        int rc = run_bytecode_vm(vm_output, CAPTURE_SIZE);
        printf("%s", vm_output);
        int pixel_failures = (rc == 0) ? check_pixel_assertions("Bytecode VM", pixel_asserts, pixel_assert_count) : 0;
        if (rc == 2) printf("\n--- Timed Out ---\n");
        else if (pixel_failures) printf("\n--- Pixel Assertions Failed ---\n");
        else printf("\n--- Done ---\n");
        if (rc == 2) return 124;
        return (rc != 0 || pixel_failures) ? 1 : 0;
    }

    if (mode == 3) {
        /* Debug mode: compile only, show stats + disassembly */
        printf("--- Debug: %s ---\n\n", filename);

        PrepareProgram(1);

        BCCompiler cs_local;
        BCCompiler *cs = &cs_local;
        if (bc_compiler_alloc(cs) != 0) {
            printf("ERROR: Cannot allocate compiler\n");
            return 1;
        }

        bc_compiler_init(cs);
        int err = bc_compile(cs, ProgMemory, PSize);
        if (err) {
            printf("COMPILE ERROR at line %d: %s\n\n",
                   cs->error_line, cs->error_msg);
        }

        bc_dump_stats(cs);
        if (!err) {
            bc_disassemble(cs);
        }
        bc_compiler_free(cs);
        return err ? 1 : 0;
    }

    /* Comparison mode: run both and compare */
    printf("Running: %s\n\n", filename);

    printf("--- Interpreter ---\n");
    int r1 = run_interpreter(interp_output, CAPTURE_SIZE);
    int p1 = (r1 == 0) ? check_pixel_assertions("Interpreter", pixel_asserts, pixel_assert_count) : 0;

    /* Re-load the program (interpreter may have modified ProgMemory) */
    load_basic_file(filename);

    printf("--- Bytecode VM ---\n");
    int r2 = run_bytecode_vm(vm_output, CAPTURE_SIZE);
    int p2 = (r2 == 0) ? check_pixel_assertions("Bytecode VM", pixel_asserts, pixel_assert_count) : 0;

    /* Compare */
    printf("\n--- Results ---\n");
    printf("Interpreter: %s\n", r1 == 0 ? "OK" : (r1 == 2 ? "TIMEOUT" : "ERROR"));
    printf("Bytecode VM: %s\n", r2 == 0 ? "OK" : (r2 == 2 ? "TIMEOUT" : "ERROR"));
    if (pixel_assert_count > 0) {
        printf("Interpreter pixels: %s\n", p1 == 0 ? "OK" : "FAIL");
        printf("Bytecode VM pixels: %s\n", p2 == 0 ? "OK" : "FAIL");
    }

    if (r1 != 0 || r2 != 0 || p1 != 0 || p2 != 0) {
        printf("\nInterpreter output:\n---\n%s\n---\n\n", interp_output);
        printf("Bytecode VM output:\n---\n%s\n---\n\n", vm_output);
        return 1;
    }

    if (strcmp(interp_output, vm_output) == 0) {
        printf("Output: MATCH\n");
        printf("\nOutput (%d chars):\n%s\n", (int)strlen(interp_output), interp_output);
        return 0;
    } else {
        printf("Output: MISMATCH!\n\n");
        printf("Interpreter output:\n---\n%s\n---\n\n", interp_output);
        printf("Bytecode VM output:\n---\n%s\n---\n\n", vm_output);

        /* Find first difference */
        int pos = 0;
        while (interp_output[pos] && vm_output[pos] && interp_output[pos] == vm_output[pos]) pos++;
        printf("First difference at position %d\n", pos);
        printf("  Interpreter: 0x%02X '%c'\n",
               (unsigned char)interp_output[pos],
               interp_output[pos] >= 32 ? interp_output[pos] : '.');
        printf("  Bytecode VM: 0x%02X '%c'\n",
               (unsigned char)vm_output[pos],
               vm_output[pos] >= 32 ? vm_output[pos] : '.');

        return 1;
    }
}
