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
extern void *GetTempMemory(int NbrBytes);
extern void ClearTempMemory(void);

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

        /* Skip arrays — they're handled in the array sync loop below.
         * Check both allocated data (already DIMmed) and the is_array flag
         * (not yet DIMmed but will be).  Without this, findvar would create
         * a scalar entry that later conflicts with the array dimensions. */
        if (vm->arrays[i].data || slot->is_array) continue;

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

        /* Resolve the MMBasic vartbl entry for this array.
         *
         * findvar("name%", V_FIND) fails if the variable already exists
         * with dimensions (e.g. from a prior RUN) because dnbr=0 != i.
         * findvar("name%()", V_FIND|V_EMPTY_OK) fails if the variable is
         * new because dnbr=-1 and i=0.
         *
         * Strategy: try V_EMPTY_OK first (handles existing arrays).
         * If that longjmps with an error, fall back to plain V_FIND
         * (handles new variables), then set dims manually. */
        unsigned char namebuf[MAXVARLEN + 4];
        int nlen = strlen(slot->name);
        memcpy(namebuf, slot->name, nlen);
        namebuf[nlen] = 0;

        BCArray *arr = &vm->arrays[i];

        if (!slot_map_initialized || slot_to_vartbl[i] < 0) {
            /* Try with empty parens first — works for existing arrays.
             * V_NOFIND_NULL avoids emitting a recoverable "... is not declared"
             * error when the variable is new and we need to create the bridge
             * entry ourselves. */
            namebuf[nlen] = '(';
            namebuf[nlen + 1] = ')';
            namebuf[nlen + 2] = 0;
            if (findvar(namebuf, V_FIND | V_EMPTY_OK | V_NOFIND_NULL) != NULL) {
                slot_to_vartbl[i] = g_VarIndex;
            } else {
                /* Variable is new. Create it as a scalar shell and then
                 * attach the VM-owned array dimensions/data below. */
                namebuf[nlen] = 0;
                findvar(namebuf, V_FIND);
                slot_to_vartbl[i] = g_VarIndex;
            }

            /* Set/update dimensions */
            struct s_vartbl *v = &g_vartbl[slot_to_vartbl[i]];
            for (int d = 0; d < MAXDIM; d++) {
                v->dims[d] = (d < arr->ndims) ? arr->dims[d] : 0;
            }
        }

        int vi = slot_to_vartbl[i];
        struct s_vartbl *v = &g_vartbl[vi];

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

static int sync_vm_locals_to_mmbasic(BCVMState *vm, int *local_map) {
    BCCompiler *cs = vm->compiler;
    if (!cs || vm->csp <= 0) return 0;

    BCCallFrame *cf = &vm->call_stack[vm->csp - 1];
    if (cf->subfun_idx >= cs->subfun_count) return 0;

    BCSubFun *sf = &cs->subfuns[cf->subfun_idx];
    uint16_t locals_base = cs->subfun_locals_base[cf->subfun_idx];
    if (sf->nlocals == 0) return 0;

    g_LocalIndex++;
    for (int i = 0; i < VM_MAX_LOCALS; i++) local_map[i] = -1;

    for (uint16_t i = 0; i < sf->nlocals; i++) {
        int slot = vm->frame_base + i;
        if (slot < 0 || slot >= VM_MAX_LOCALS) continue;

        BCLocalMeta *meta = &cs->local_meta[locals_base + i];
        if (!meta->name[0] || !isnamestart((unsigned char)meta->name[0])) continue;

        unsigned char namebuf[MAXVARLEN + 4];
        int nlen = strlen(meta->name);
        memcpy(namebuf, meta->name, nlen);
        namebuf[nlen] = 0;

        if (meta->is_array) {
            BCArray *arr = &vm->local_arrays[slot];
            namebuf[nlen] = '(';
            namebuf[nlen + 1] = ')';
            namebuf[nlen + 2] = 0;
            findvar(namebuf, V_FIND | V_DIM_VAR | V_LOCAL | V_EMPTY_OK | meta->type);
            local_map[slot] = g_VarIndex;

            if (arr->data) {
                struct s_vartbl *v = &g_vartbl[g_VarIndex];
                for (int d = 0; d < MAXDIM; d++) {
                    v->dims[d] = (d < arr->ndims) ? arr->dims[d] : 0;
                }
                if (meta->type == T_INT) v->val.ia = (long long int *)arr->data;
                else if (meta->type == T_NBR) v->val.fa = (MMFLOAT *)arr->data;
                else if (meta->type == T_STR) v->val.s = (unsigned char *)arr->data;
            }
            continue;
        }

        findvar(namebuf, V_FIND | V_DIM_VAR | V_LOCAL | V_EMPTY_OK | meta->type);
        local_map[slot] = g_VarIndex;
        struct s_vartbl *v = &g_vartbl[g_VarIndex];
        if (meta->type == T_INT) {
            v->val.i = vm->locals[slot].i;
        } else if (meta->type == T_NBR) {
            v->val.f = vm->locals[slot].f;
        } else if (meta->type == T_STR && vm->locals[slot].s) {
            Mstrcpy(v->val.s, vm->locals[slot].s);
        }
    }

    return g_LocalIndex;
}

static void sync_mmbasic_locals_to_vm(BCVMState *vm, const int *local_map) {
    BCCompiler *cs = vm->compiler;
    if (!cs || vm->csp <= 0) return;

    BCCallFrame *cf = &vm->call_stack[vm->csp - 1];
    if (cf->subfun_idx >= cs->subfun_count) return;

    BCSubFun *sf = &cs->subfuns[cf->subfun_idx];
    uint16_t locals_base = cs->subfun_locals_base[cf->subfun_idx];
    for (uint16_t i = 0; i < sf->nlocals; i++) {
        int slot = vm->frame_base + i;
        if (slot < 0 || slot >= VM_MAX_LOCALS) continue;
        if (local_map[slot] < 0) continue;

        BCLocalMeta *meta = &cs->local_meta[locals_base + i];
        if (meta->is_array) continue;

        struct s_vartbl *v = &g_vartbl[local_map[slot]];
        vm->local_types[slot] = meta->type;
        if (meta->type == T_INT) {
            vm->locals[slot].i = v->val.i;
        } else if (meta->type == T_NBR) {
            vm->locals[slot].f = v->val.f;
        } else if (meta->type == T_STR && v->val.s) {
            uint8_t *temp = &vm->str_temp[vm->str_temp_idx % 4][0];
            vm->str_temp_idx++;
            Mstrcpy(temp, v->val.s);
            vm->locals[slot].s = temp;
        }
    }
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
    int saved_local_index = g_LocalIndex;
    int local_map[VM_MAX_LOCALS];
    int bridge_level = 0;

    /* Sync VM variables to MMBasic table so the command can see them */
    sync_vm_to_mmbasic(vm);
    bridge_level = sync_vm_locals_to_mmbasic(vm, local_map);

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
    sync_mmbasic_locals_to_vm(vm, local_map);
    sync_mmbasic_to_vm(vm);
    if (bridge_level) ClearVars(bridge_level, true);
    g_LocalIndex = saved_local_index;

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
    int saved_local_index = g_LocalIndex;
    int local_map[VM_MAX_LOCALS];
    int bridge_level = 0;

    /* Sync VM variables to MMBasic table so the function can see them */
    sync_vm_to_mmbasic(vm);
    bridge_level = sync_vm_locals_to_mmbasic(vm, local_map);

    /* Set up globals for the function handler */
    ep = (unsigned char *)ptr_val;
    targ = ret_type;

    /* Look up the function in the token table and call it.
     * fun_idx is the token table index. */
    tokentbl[fun_idx].fptr();

    /* Sync any modified variables back to VM */
    sync_mmbasic_locals_to_vm(vm, local_map);
    sync_mmbasic_to_vm(vm);
    if (bridge_level) ClearVars(bridge_level, true);
    g_LocalIndex = saved_local_index;

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
#ifdef MMBASIC_HOST
#define FRUN_DBG(s)       ((void)0)
#define FRUN_DBGF(fmt...) ((void)0)
#else
#define FRUN_DBG(s)       MMPrintString(s)
#define FRUN_DBGF(fmt...) do { char _b[80]; snprintf(_b, sizeof(_b), fmt); MMPrintString(_b); } while(0)
#endif

void cmd_frun(void) {
    int err;
#ifndef MMBASIC_HOST
    bc_debug_enabled = 1;  /* enable line/cmd tracing on device */
    FRUN_DBG("FRUN: entry\r\n");
#endif
    bc_crash_checkpoint(BC_CK_FRUN_ENTRY, "entry");

    /* Heap-allocate the compiler and VM structs themselves.
     * BCVMState is ~6 KB which overflows the 4 KB device stack. */
    BCCompiler *cs = (BCCompiler *)BC_ALLOC(sizeof(BCCompiler));
    BCVMState  *vm = (BCVMState  *)BC_ALLOC(sizeof(BCVMState));
    if (!cs || !vm) {
        if (cs) BC_FREE(cs);
        if (vm) BC_FREE(vm);
        error("Not enough memory for FRUN");
        return;
    }
    memset(cs, 0, sizeof(BCCompiler));
    memset(vm, 0, sizeof(BCVMState));

    FRUN_DBG("FRUN: structs allocated\r\n");
    bc_crash_checkpoint(BC_CK_FRUN_ALLOC_CS, "structs ok");

    if (bc_compiler_alloc(cs) != 0) {
        BC_FREE(cs);
        BC_FREE(vm);
        error("Not enough memory for FRUN compiler");
        return;
    }

    FRUN_DBG("FRUN: compiler allocated\r\n");
    bc_crash_checkpoint(BC_CK_FRUN_COMP_ALLOC, "compiler alloc");

    if (bc_vm_alloc(vm) != 0) {
        bc_compiler_free(cs);
        BC_FREE(cs);
        BC_FREE(vm);
        error("Not enough memory for FRUN VM");
        return;
    }

    FRUN_DBG("FRUN: vm allocated\r\n");
    bc_crash_checkpoint(BC_CK_FRUN_VM_ALLOC, "vm alloc");

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

    FRUN_DBGF("FRUN: compiling %d bytes...\r\n", compile_size);
    bc_crash_checkpoint(BC_CK_FRUN_COMPILE, "compiling");
    err = bc_compile(cs, ProgMemory, compile_size);
    if (err) {
        char msg[160];
        snprintf(msg, sizeof(msg), "FRUN compile error at line %d: %s",
                 cs->error_line, cs->error_msg);
        bc_dump_stats(cs);
        bc_vm_free(vm);
        bc_compiler_free(cs);
        BC_FREE(cs);
        BC_FREE(vm);
        error(msg);
        return;
    }

    FRUN_DBGF("FRUN: compiled OK, %d bytes code, %d slots\r\n",
              (int)cs->code_len, (int)cs->slot_count);

    if (bc_debug_enabled) {
        bc_dump_stats(cs);
        bc_disassemble(cs);
    }

    /* Initialize and run the VM */
    bc_crash_checkpoint(BC_CK_FRUN_VM_INIT, "vm init");
    bc_vm_init(vm, cs);
    bc_bridge_reset_sync();

    FRUN_DBG("FRUN: executing...\r\n");
    bc_crash_checkpoint(BC_CK_FRUN_EXECUTE, "executing");

    /* Save the main loop's mark jmp_buf.  cmd_frun returns to the caller
     * (MMBasic main loop) which relies on mark for error recovery.  If we
     * overwrite mark without restoring it, longjmp(mark) after we return
     * would jump into our destroyed stack frame → bus fault. */
    jmp_buf saved_mark;
    memcpy(saved_mark, mark, sizeof(jmp_buf));

    if (setjmp(mark) == 0) {
        bc_vm_execute(vm);
    } else {
        /* VM exited via error/longjmp — fall through to cleanup */
        FRUN_DBGF("FRUN: VM exited via longjmp at line %d\r\n",
                  (int)vm->current_line);
    }

    memcpy(mark, saved_mark, sizeof(jmp_buf));

    FRUN_DBG("FRUN: cleanup\r\n");
    bc_crash_checkpoint(BC_CK_FRUN_CLEANUP, "cleanup");

    /* Clean up */
    /* Free any array data allocated by the VM (via BC_ALLOC in OP_DIM_ARR).
     * String element buffers were allocated via GetTempMemory and will be
     * cleaned up by ClearTempMemory or on next m_alloc. */
    for (int i = 0; i < BC_MAX_SLOTS; i++) {
        if (vm->arrays[i].data) {
            BC_FREE(vm->arrays[i].data);
        }
    }

    bc_crash_checkpoint(BC_CK_FRUN_CLEANUP + 1, "cleanup:vm_free");
    bc_vm_free(vm);

    bc_crash_checkpoint(BC_CK_FRUN_CLEANUP + 2, "cleanup:comp_free");
    bc_compiler_free(cs);

    bc_crash_checkpoint(BC_CK_FRUN_CLEANUP + 3, "cleanup:struct_free");
    BC_FREE(cs);
    BC_FREE(vm);

    FRUN_DBG("FRUN: done\r\n");
    bc_crash_checkpoint(BC_CK_FRUN_CLEANUP + 4, "cleanup:done");
    /* Successful completion — clear crash breadcrumb */
    bc_crash_clear();
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
