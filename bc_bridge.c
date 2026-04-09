/*
 * bc_bridge.c — Bridge layer between bytecode VM and existing MMBasic built-in commands/functions
 *
 * Also contains cmd_frun() and cmd_ftest() command implementations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "MMBasic.h"
#include "bytecode.h"

/* External MMBasic globals needed by bridge */
extern const struct s_tokentbl tokentbl[];
extern const struct s_tokentbl commandtbl[];
extern int CommandTableSize, TokenTableSize;
extern unsigned char *ProgMemory;
extern int PSize;
extern jmp_buf mark;
extern int cmdtoken;
extern unsigned char *cmdline;
extern unsigned char *nextstmt;
extern MMFLOAT fret;
extern long long int iret;
extern unsigned char *sret;
extern int targ;
extern unsigned char *ep;
extern MMFLOAT farg1, farg2;
extern long long int iarg1, iarg2;
extern unsigned char *sarg1, *sarg2;

/* External memory functions */
extern void *GetMemory(int size);
extern void FreeMemory(unsigned char *addr);
extern void *GetTempMemory(int NbrBytes);
extern void ClearTempMemory(void);

/* External output functions */
extern void MMPrintString(char *s);
extern void MMfputs(unsigned char *p, int filenbr);

/* External utility */
extern void CheckAbort(void);
extern void check_interrupt(void);
extern void IntToStr(char *strr, long long int nbr, unsigned int base);
extern void FloatToStr(char *p, MMFLOAT f, int m, int n, unsigned char ch);
extern void error(char *msg, ...);
extern void ClearRuntime(bool all);
extern void ClearVars(int level, bool all);
extern void PrepareProgram(int ErrAbort);

/* The test harness function (defined in bc_test.c) */
extern void bc_run_tests(void);

/* ------------------------------------------------------------------ */
/*  Variable sync: VM globals ↔ MMBasic variable table                 */
/* ------------------------------------------------------------------ */

/*
 * Cached mapping from VM slot index → g_vartbl index.
 * Built lazily on first bridge call and reused for the duration of the program.
 * -1 means "not yet resolved".
 */
static int slot_to_vartbl[BC_MAX_SLOTS];
static int slot_map_initialized = 0;

/*
 * Sync VM globals → MMBasic variable table (pre-bridge-call).
 * Creates MMBasic variables if they don't exist yet.
 */
static void sync_vm_to_mmbasic(BCVMState *vm) {
    BCCompiler *cs = vm->compiler;
    if (!cs) return;

    for (uint16_t i = 0; i < cs->slot_count; i++) {
        BCSlot *slot = &cs->slots[i];

        /* Skip slots with invalid names (e.g. tokenized keywords) */
        if (!slot->name[0] || !isnamestart((unsigned char)slot->name[0])) continue;

        /* Skip actual arrays (have allocated data) — they're handled separately below */
        if (vm->arrays[i].data) continue;

        /* Build the name string findvar() expects (already includes suffix) */
        unsigned char namebuf[MAXVARLEN + 2];
        int nlen = strlen(slot->name);
        memcpy(namebuf, slot->name, nlen);
        namebuf[nlen] = 0;

        /* Find or create the MMBasic variable */
        if (!slot_map_initialized || slot_to_vartbl[i] < 0) {
            findvar(namebuf, V_FIND);
            slot_to_vartbl[i] = g_VarIndex;
        }

        int vi = slot_to_vartbl[i];
        struct s_vartbl *v = &g_vartbl[vi];

        /* Copy value from VM → MMBasic */
        if (slot->type == T_INT) {
            v->val.i = vm->globals[i].i;
        } else if (slot->type == T_NBR) {
            v->val.f = vm->globals[i].f;
        } else if (slot->type == T_STR) {
            if (vm->globals[i].s) {
                if (!v->val.s) {
                    v->val.s = GetTempMemory(STRINGSIZE);
                }
                Mstrcpy(v->val.s, vm->globals[i].s);
            }
        }
    }

    /* Also sync arrays — copy data pointers so bridge can index them */
    for (uint16_t i = 0; i < cs->slot_count; i++) {
        BCSlot *slot = &cs->slots[i];
        if (!slot->name[0] || !isnamestart((unsigned char)slot->name[0])) continue;
        if (!slot->is_array) continue;
        if (!vm->arrays[i].data) continue;

        unsigned char namebuf[MAXVARLEN + 2];
        int nlen = strlen(slot->name);
        memcpy(namebuf, slot->name, nlen);
        namebuf[nlen] = 0;

        /* For arrays, we need to DIM them in MMBasic's table too */
        if (!slot_map_initialized || slot_to_vartbl[i] < 0) {
            /* Create the variable first (non-array, findvar handles it) */
            findvar(namebuf, V_FIND);
            slot_to_vartbl[i] = g_VarIndex;
        }

        int vi = slot_to_vartbl[i];
        struct s_vartbl *v = &g_vartbl[vi];

        /* Sync array dimensions and data pointer */
        BCArray *arr = &vm->arrays[i];
        for (int d = 0; d < MAXDIM; d++) {
            v->dims[d] = (d < arr->ndims) ? arr->dims[d] : 0;
        }

        /* Point MMBasic's array data to VM's array data */
        if (slot->type == T_INT) {
            v->val.ia = (long long int *)arr->data;
        } else if (slot->type == T_NBR) {
            v->val.fa = (MMFLOAT *)arr->data;
        } else if (slot->type == T_STR) {
            v->val.s = (unsigned char *)arr->data;
        }
    }

    slot_map_initialized = 1;
}

/*
 * Sync MMBasic variable table → VM globals (post-bridge-call).
 * Only syncs variables that were previously mapped.
 */
static void sync_mmbasic_to_vm(BCVMState *vm) {
    BCCompiler *cs = vm->compiler;
    if (!cs || !slot_map_initialized) return;

    for (uint16_t i = 0; i < cs->slot_count; i++) {
        BCSlot *slot = &cs->slots[i];
        if (!slot->name[0] || !isnamestart((unsigned char)slot->name[0])) continue;
        if (vm->arrays[i].data) continue;  /* arrays share data pointer, no copy needed */
        if (slot_to_vartbl[i] < 0) continue;

        struct s_vartbl *v = &g_vartbl[slot_to_vartbl[i]];

        if (slot->type == T_INT) {
            vm->globals[i].i = v->val.i;
        } else if (slot->type == T_NBR) {
            vm->globals[i].f = v->val.f;
        } else if (slot->type == T_STR) {
            if (v->val.s) {
                /* Copy string back to VM's temp storage */
                uint8_t *temp = &vm->str_temp[vm->str_temp_idx % 4][0];
                vm->str_temp_idx++;
                Mstrcpy(temp, v->val.s);
                vm->globals[i].s = temp;
            }
        }
    }
}

/*
 * Reset the slot map cache (called at start of each FRUN).
 */
void bc_bridge_reset_sync(void) {
    memset(slot_to_vartbl, -1, sizeof(slot_to_vartbl));
    slot_map_initialized = 0;
}

/*
 * Bridge: call a built-in command from the VM
 *
 * This is a compatibility layer. The built-in command handlers expect:
 *   - cmdline: pointer to arguments as tokenized text
 *   - cmdtoken: the command index
 *   - nextstmt: pointer to next statement
 *
 * For the bridge, we can't perfectly reconstruct the tokenized text from
 * stack values. Instead, for bridged commands we pass through to the
 * original interpreter's handling of that command line.
 *
 * The approach: the compiler stores the original cmdline pointer in the
 * bytecode stream alongside OP_BUILTIN_CMD. The VM passes it through.
 */
void bc_bridge_call_cmd(BCVMState *vm, uint16_t cmd_idx) {
    /* Read the pointer to the original tokenized command arguments
     * that was embedded after the opcode by the compiler.
     * The compiler emits: OP_BUILTIN_CMD [idx:16] [orig_cmdline_ptr:ptr]
     * The VM already read idx, now read the pointer. */
    uintptr_t ptr_val;
    memcpy(&ptr_val, vm->pc, sizeof(ptr_val));
    vm->pc += sizeof(ptr_val);

    /* Save VM context */
    int saved_cmdtoken = cmdtoken;
    unsigned char *saved_cmdline = cmdline;
    unsigned char *saved_nextstmt = nextstmt;

    /* Sync VM variables to MMBasic table so the command can see them */
    sync_vm_to_mmbasic(vm);

    /* Set up globals for the command handler */
    cmdtoken = cmd_idx;
    cmdline = (unsigned char *)ptr_val;
    /* nextstmt needs to point past the command's arguments */
    unsigned char *p = cmdline;
    while (*p) p++;  /* skip to end of element */
    nextstmt = p;

    /* Call the command handler */
    commandtbl[cmd_idx].fptr();

    /* Sync any modified variables back to VM */
    sync_mmbasic_to_vm(vm);

    /* Restore VM context */
    cmdtoken = saved_cmdtoken;
    cmdline = saved_cmdline;
    nextstmt = saved_nextstmt;

    /* Clear temp memory allocated by the command */
    ClearTempMemory();
}

/*
 * Bridge: call a built-in function from the VM
 *
 * Built-in functions expect:
 *   - ep: pointer to arguments (between parentheses)
 *   - targ: expected return type
 *   - Result returned in fret/iret/sret
 */
void bc_bridge_call_fun(BCVMState *vm, uint16_t fun_idx, uint8_t nargs, uint8_t ret_type) {
    /* Read the pointer to the original function arguments */
    uintptr_t ptr_val;
    memcpy(&ptr_val, vm->pc, sizeof(ptr_val));
    vm->pc += sizeof(ptr_val);

    /* Save VM context */
    int saved_targ = targ;
    unsigned char *saved_ep = ep;
    MMFLOAT saved_fret = fret;
    long long int saved_iret = iret;
    unsigned char *saved_sret = sret;

    /* Sync VM variables to MMBasic table so the function can see them */
    sync_vm_to_mmbasic(vm);

    /* Set up globals for the function handler */
    ep = (unsigned char *)ptr_val;
    targ = ret_type;

    /* Look up the function in the token table and call it.
     * fun_idx is the token table index. */
    tokentbl[fun_idx].fptr();

    /* Sync any modified variables back to VM */
    sync_mmbasic_to_vm(vm);

    /* Push result onto VM stack */
    if (targ & T_INT) {
        vm->sp++;
        vm->stack[vm->sp].i = iret;
        vm->stack_types[vm->sp] = T_INT;
    } else if (targ & T_NBR) {
        vm->sp++;
        vm->stack[vm->sp].f = fret;
        vm->stack_types[vm->sp] = T_NBR;
    } else if (targ & T_STR) {
        /* Copy string to VM temp storage */
        uint8_t *temp = &vm->str_temp[vm->str_temp_idx % 4][0];
        vm->str_temp_idx++;
        if (sret) {
            int len = *sret;
            memcpy(temp, sret, len + 1);
        } else {
            temp[0] = 0;
        }
        vm->sp++;
        vm->stack[vm->sp].s = temp;
        vm->stack_types[vm->sp] = T_STR;
    }

    /* Restore VM context */
    targ = saved_targ;
    ep = saved_ep;
    fret = saved_fret;
    iret = saved_iret;
    sret = saved_sret;

    ClearTempMemory();
}


/*
 * cmd_frun() — The FRUN command
 *
 * Compiles the current program to bytecode and executes it on the VM.
 */
void cmd_frun(void) {
    BCCompiler *cs;
    BCVMState *vm;
    int err;

    /* Allocate compiler and VM state via malloc — these structures are too
     * large (BCCompiler ~350KB, BCVMState ~80KB) for the MMBasic heap
     * (128KB on RP2040, 300KB on RP2350). calloc zeroes the memory. */
    cs = (BCCompiler *)calloc(1, sizeof(BCCompiler));
    if (!cs) {
        error("Not enough memory for FRUN compiler");
        return;
    }

    vm = (BCVMState *)calloc(1, sizeof(BCVMState));
    if (!vm) {
        free(cs);
        error("Not enough memory for FRUN VM");
        return;
    }

    /* Compile the program */
    bc_compiler_init(cs);
    err = bc_compile(cs, ProgMemory, PSize);
    if (err) {
        char msg[160];
        snprintf(msg, sizeof(msg), "FRUN compile error at line %d: %s",
                 cs->error_line, cs->error_msg);
        free(vm);
        free(cs);
        error(msg);
        return;
    }

    /* Initialize and run the VM */
    bc_vm_init(vm, cs);
    bc_bridge_reset_sync();

    if (setjmp(mark) == 0) {
        bc_vm_execute(vm);
    }

    /* Clean up */
    /* Free any array data allocated by the VM (via calloc in OP_DIM_ARR).
     * String element buffers were allocated via GetTempMemory and will be
     * cleaned up by ClearTempMemory or on next m_alloc. */
    for (int i = 0; i < BC_MAX_SLOTS; i++) {
        if (vm->arrays[i].data) {
            free(vm->arrays[i].data);
        }
    }

    free(vm);
    free(cs);
}


/*
 * cmd_ftest() — The FTEST command
 *
 * Runs the bytecode VM test suite, comparing RUN vs FRUN output.
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
