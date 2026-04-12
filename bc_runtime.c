/*
 * bc_runtime.c - VM command entrypoints and host test hooks.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MMBasic.h"
#include "bytecode.h"
#include "bc_alloc.h"
#include "bc_source.h"
#include "vm_sys_file.h"
#include "vm_sys_graphics.h"

extern jmp_buf mark;

/* External output functions */
extern void MMPrintString(char *s);

/* External utility */
extern void error(char *msg, ...);

#ifdef MMBASIC_HOST
#define VMRUN_DBG(s)       ((void)0)
#define VMRUN_DBGF(fmt...) ((void)0)
#else
#define VMRUN_DBG(s)       MMPrintString(s)
#define VMRUN_DBGF(fmt...) do { char _b[80]; snprintf(_b, sizeof(_b), fmt); MMPrintString(_b); } while(0)
#endif

#ifndef MMBASIC_HOST
static BCCompiler device_compiler;
static BCVMState device_vm;
#endif

/*
 * Compiles raw BASIC source directly to bytecode and executes it on the VM.
 * This is the VM-owned frontend path; it must not depend on legacy tokenised
 * ProgMemory or interpreter command/function dispatch.  The caller owns the
 * source buffer; on device it may live in bc_alloc's arena, so this function
 * must not reset the arena on normal entry/exit.
 */
void bc_run_source_string(const char *source, const char *source_name) {
    int err;
    bc_fastgfx_reset();
    vm_sys_file_reset();
    vm_sys_graphics_reset();
    bc_crash_checkpoint(BC_CK_VM_ENTRY, "source entry");

#ifdef MMBASIC_HOST
    BCCompiler *cs = (BCCompiler *)BC_ALLOC(sizeof(BCCompiler));
    BCVMState  *vm = (BCVMState  *)BC_ALLOC(sizeof(BCVMState));
    if (!cs || !vm) {
        if (cs) BC_FREE(cs);
        if (vm) BC_FREE(vm);
        bc_alloc_reset();
        error("Not enough memory for VM");
        return;
    }
#else
    BCCompiler *cs = &device_compiler;
    BCVMState  *vm = &device_vm;
#endif
    memset(cs, 0, sizeof(BCCompiler));
    memset(vm, 0, sizeof(BCVMState));

    if (bc_compiler_alloc(cs) != 0) {
#ifdef MMBASIC_HOST
        BC_FREE(cs);
        BC_FREE(vm);
#endif
        bc_alloc_reset();
        error("Not enough memory for VM compiler");
        return;
    }

    bc_compiler_init(cs);
    err = bc_compile_source(cs, source, source_name);
    if (err) {
        char msg[160];
        snprintf(msg, sizeof(msg), "VM source compile error at line %d: %.100s",
                 cs->error_line, cs->error_msg);
        bc_compiler_free(cs);
#ifdef MMBASIC_HOST
        BC_FREE(cs);
        BC_FREE(vm);
#endif
        bc_alloc_reset();
        error("$", msg);
        return;
    }

    if (bc_alloc_owns(source)) {
        BC_FREE((void *)source);
        source = NULL;
    }

    bc_compiler_compact(cs);
    bc_compile_release_all();

    if (bc_vm_alloc(vm) != 0) {
        bc_compiler_free(cs);
#ifdef MMBASIC_HOST
        BC_FREE(cs);
        BC_FREE(vm);
#endif
        bc_alloc_reset();
        error("Not enough memory for VM runtime");
        return;
    }

    bc_vm_init(vm, cs);

    jmp_buf saved_mark;
    memcpy(saved_mark, mark, sizeof(jmp_buf));

    if (setjmp(mark) == 0) {
        bc_vm_execute(vm);
    }

    memcpy(mark, saved_mark, sizeof(jmp_buf));

    bc_vm_free(vm);
    bc_compiler_free(cs);
    bc_compile_release_all();
#ifdef MMBASIC_HOST
    BC_FREE(cs);
    BC_FREE(vm);
#endif
    vm_sys_file_reset();
    vm_sys_graphics_reset();
    bc_fastgfx_reset();
    bc_crash_clear();
}

/*
 * Helper: capture VM output to a buffer
 *
 * Call before bc_vm_execute to redirect PRINT output to a string buffer.
 */
void bc_vm_start_capture(BCVMState *vm, char *buf, int capacity) {
    vm->capture_buf = buf;
    vm->capture_len = 0;
    vm->capture_cap = capacity;
    if (capacity > 0) buf[0] = '\0';
}

/*
 * Helper: append to capture buffer (used by VM print operations)
 */
void bc_vm_capture_write(BCVMState *vm, const char *text, int len) {
    if (!vm->capture_buf) return;
    if (vm->capture_len + len >= vm->capture_cap) {
        len = vm->capture_cap - vm->capture_len - 1;
        if (len <= 0) return;
    }
    memcpy(vm->capture_buf + vm->capture_len, text, len);
    vm->capture_len += len;
    vm->capture_buf[vm->capture_len] = '\0';
}

void bc_vm_capture_char(BCVMState *vm, char c) {
    bc_vm_capture_write(vm, &c, 1);
}

void bc_vm_capture_string(BCVMState *vm, const char *s) {
    bc_vm_capture_write(vm, s, strlen(s));
}
