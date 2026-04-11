/*
 * bc_runtime.c - VM command entrypoints and host test hooks.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MMBasic.h"
#include "bytecode.h"

extern unsigned char *ProgMemory;
extern jmp_buf mark;

#ifdef MMBASIC_HOST
#define BC_ALLOC(sz)   calloc(1, (sz))
#define BC_FREE(p)     free((p))
#else
extern void *GetMemory(int size);
extern void FreeMemory(unsigned char *addr);
#define BC_ALLOC(sz)   GetMemory((sz))
#define BC_FREE(p)     FreeMemory((unsigned char *)(p))
#endif

/* External output functions */
extern void MMPrintString(char *s);

/* External utility */
extern void error(char *msg, ...);

/* The test harness function (defined in bc_test.c) */
extern void bc_run_tests(void);

#ifdef MMBASIC_HOST
#define VMRUN_DBG(s)       ((void)0)
#define VMRUN_DBGF(fmt...) ((void)0)
#else
#define VMRUN_DBG(s)       MMPrintString(s)
#define VMRUN_DBGF(fmt...) do { char _b[80]; snprintf(_b, sizeof(_b), fmt); MMPrintString(_b); } while(0)
#endif

/*
 * Compiles the current tokenised program to bytecode and executes it on the VM.
 */
void bc_run_current_program(void) {
    int err;
    bc_crash_checkpoint(BC_CK_VM_ENTRY, "entry");

    /* Heap-allocate the compiler and VM structs themselves.
     * BCVMState is ~6 KB which overflows the 4 KB device stack. */
    BCCompiler *cs = (BCCompiler *)BC_ALLOC(sizeof(BCCompiler));
    BCVMState  *vm = (BCVMState  *)BC_ALLOC(sizeof(BCVMState));
    if (!cs || !vm) {
        if (cs) BC_FREE(cs);
        if (vm) BC_FREE(vm);
        error("Not enough memory for VM");
        return;
    }
    memset(cs, 0, sizeof(BCCompiler));
    memset(vm, 0, sizeof(BCVMState));

    bc_crash_checkpoint(BC_CK_VM_ALLOC_CS, "structs ok");

    if (bc_compiler_alloc(cs) != 0) {
        BC_FREE(cs);
        BC_FREE(vm);
        error("Not enough memory for VM compiler");
        return;
    }

    bc_crash_checkpoint(BC_CK_VM_COMP_ALLOC, "compiler alloc");

    if (bc_vm_alloc(vm) != 0) {
        bc_compiler_free(cs);
        BC_FREE(cs);
        BC_FREE(vm);
        error("Not enough memory for VM runtime");
        return;
    }

    bc_crash_checkpoint(BC_CK_VM_ALLOC, "vm alloc");

    /* Compile the program */
    bc_compiler_init(cs);

    /* PSize is unreliable on device (often 0).  The regular interpreter
     * never uses it — it scans for the double-null terminator.  We do
     * the same: walk ProgMemory element-by-element to find the true end.
     * Must handle T_LINENBR specially (3 bytes) since line-number bytes
     * can be 0x00 and would fool a naive scan. */
    int compile_size;
    {
        unsigned char *ep = ProgMemory;
        while (1) {
            if (*ep == 0) {
                if (ep[1] == 0 || ep[1] == 0xFF) break;  /* end of program */
                ep++;           /* element separator — skip */
                continue;
            }
            if (*ep == T_NEWLINE) { ep++; continue; }
            if (*ep == T_LINENBR) { ep += 3; continue; }   /* token + hi + lo */
            if (*ep == T_LABEL)   { ep += ep[1] + 2; continue; }
            ep++;               /* any other byte */
        }
        compile_size = (int)(ep - ProgMemory) + 2;  /* include double-null */
    }

    bc_crash_checkpoint(BC_CK_VM_COMPILE, "compiling");
    err = bc_compile(cs, ProgMemory, compile_size);
    if (err) {
        char msg[160];
        snprintf(msg, sizeof(msg), "VM compile error at line %d: %s",
                 cs->error_line, cs->error_msg);
        bc_dump_stats(cs);
        bc_vm_free(vm);
        bc_compiler_free(cs);
        BC_FREE(cs);
        BC_FREE(vm);
        error("$", msg);
        return;
    }

#ifndef MMBASIC_HOST
    VMRUN_DBGF("VM: %d bytes, %d vars\r\n",
              (int)cs->code_len, (int)cs->slot_count);
#endif

    /* Compact: free compile-only arrays + shrink runtime arrays to actual size.
     * Reclaims >100KB for program use (display buffers, large arrays, etc.) */
    bc_compiler_compact(cs);

    /* Initialize and run the VM */
    bc_crash_checkpoint(BC_CK_VM_INIT, "vm init");
    bc_vm_init(vm, cs);

    bc_crash_checkpoint(BC_CK_VM_EXECUTE, "executing");

    /* Save the main loop's mark jmp_buf.  The VM returns to the caller
     * (MMBasic main loop) which relies on mark for error recovery.  If we
     * overwrite mark without restoring it, longjmp(mark) after we return
     * would jump into our destroyed stack frame → bus fault. */
    jmp_buf saved_mark;
    memcpy(saved_mark, mark, sizeof(jmp_buf));

    if (setjmp(mark) == 0) {
        bc_vm_execute(vm);
    } else {
        /* VM exited via error/longjmp — fall through to cleanup */
    }

    memcpy(mark, saved_mark, sizeof(jmp_buf));

    bc_crash_checkpoint(BC_CK_VM_CLEANUP, "cleanup");

    /* Clean up */
    /* Free any array data allocated by the VM (via BC_ALLOC in OP_DIM_ARR).
     * String element buffers were allocated via GetTempMemory and will be
     * cleaned up by ClearTempMemory or on next m_alloc. */
    for (int i = 0; i < BC_MAX_SLOTS; i++) {
        if (vm->arrays[i].data) {
            BC_FREE(vm->arrays[i].data);
        }
    }

    bc_crash_checkpoint(BC_CK_VM_CLEANUP + 1, "cleanup:vm_free");
    bc_vm_free(vm);

    bc_crash_checkpoint(BC_CK_VM_CLEANUP + 2, "cleanup:comp_free");
    bc_compiler_free(cs);

    bc_crash_checkpoint(BC_CK_VM_CLEANUP + 3, "cleanup:struct_free");
    BC_FREE(cs);
    BC_FREE(vm);

    bc_crash_checkpoint(BC_CK_VM_CLEANUP + 4, "cleanup:done");
    /* Successful completion — clear crash breadcrumb */
    bc_crash_clear();
}

/*
 * cmd_ftest() — The FTEST command
 *
 * Runs the bytecode VM test suite.
 */
void cmd_ftest(void) {
    bc_run_tests();
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
