/*
 * bc_bridge.c — Bridge between bytecode VM and interpreter command/function handlers.
 *
 * When the VM encounters an OP_BRIDGE_CMD, it has pre-tokenized bytes
 * (produced by tokenise() at compile time). This module sets up the
 * interpreter's global state (cmdtoken, cmdline, nextstmt) and calls
 * the appropriate command handler, then syncs variables back.
 */

#include <stdio.h>
#include <string.h>
#include "MMBasic.h"
#include "Memory.h"
#include "bytecode.h"
#include "bc_alloc.h"

/* ------------------------------------------------------------------ */
/*  Variable sync: VM globals <-> MMBasic variable table               */
/* ------------------------------------------------------------------ */

/*
 * Cached mapping from VM slot index -> g_vartbl index.
 * Built lazily on first bridge call and reused for the program's lifetime.
 * -1 means "not yet resolved".
 */
static int slot_to_vartbl[BC_MAX_SLOTS];
static int slot_map_initialized = 0;

void bc_bridge_reset_sync(void) {
    memset(slot_to_vartbl, -1, sizeof(slot_to_vartbl));
    slot_map_initialized = 0;
}

/*
 * Sync VM globals -> MMBasic variable table (pre-bridge-call).
 * Creates MMBasic variables if they don't exist yet.
 */
/* Build the findvar action flags that register a VM-side slot in
 * g_vartbl with the correct type. Pass T_IMPLIED so findvar marks the
 * entry as "declared-without-suffix" — a later bare-name lookup from
 * inside a bridged command handler (e.g. getint(p="W") in cmd_save)
 * passes its type check against DefaultType | T_IMPLIED instead of
 * erroring with "Different type already declared".
 *
 * Mirrors how the interpreter itself stores DIM-As-type variables:
 * `Dim W As Integer` stores W with type = T_INT | T_IMPLIED. */
static int sync_find_action(uint8_t slot_type) {
    int base = V_FIND | T_IMPLIED;
    switch (slot_type) {
        case T_INT: return base | T_INT;
        case T_NBR: return base | T_NBR;
        case T_STR: return base | T_STR;
        default:    return V_FIND;
    }
}

static void sync_vm_to_mmbasic(BCVMState *vm) {
    BCCompiler *cs = vm->compiler;
    if (!cs) return;

    for (uint16_t i = 0; i < cs->slot_count; i++) {
        BCSlot *slot = &cs->slots[i];

        if (!slot->name[0] || !isnamestart((unsigned char)slot->name[0])) continue;
        if (vm->arrays[i].data || slot->is_array) continue;

        unsigned char namebuf[MAXVARLEN + 2];
        int nlen = strlen(slot->name);
        memcpy(namebuf, slot->name, nlen);
        namebuf[nlen] = 0;

        if (!slot_map_initialized || slot_to_vartbl[i] < 0) {
            findvar(namebuf, sync_find_action(slot->type));
            slot_to_vartbl[i] = g_VarIndex;
        }

        int vi = slot_to_vartbl[i];
        struct s_vartbl *v = &g_vartbl[vi];

        if (slot->type == T_INT) {
            v->val.i = vm->globals[i].i;
        } else if (slot->type == T_NBR) {
            v->val.f = vm->globals[i].f;
        } else if (slot->type == T_STR) {
            if (vm->globals[i].s) {
                v->val.s = GetTempMemory(STRINGSIZE);
                Mstrcpy(v->val.s, vm->globals[i].s);
            }
        }
    }

    /* Sync arrays — point MMBasic's array data to VM's array data */
    for (uint16_t i = 0; i < cs->slot_count; i++) {
        BCSlot *slot = &cs->slots[i];
        if (!slot->name[0] || !isnamestart((unsigned char)slot->name[0])) continue;
        if (!slot->is_array) continue;
        if (!vm->arrays[i].data) continue;

        unsigned char namebuf[MAXVARLEN + 4];
        int nlen = strlen(slot->name);
        memcpy(namebuf, slot->name, nlen);
        namebuf[nlen] = 0;

        BCArray *arr = &vm->arrays[i];
        int action = sync_find_action(slot->type);

        if (!slot_map_initialized || slot_to_vartbl[i] < 0) {
            namebuf[nlen] = '(';
            namebuf[nlen + 1] = ')';
            namebuf[nlen + 2] = 0;
            if (findvar(namebuf, action | V_EMPTY_OK | V_NOFIND_NULL) != NULL) {
                slot_to_vartbl[i] = g_VarIndex;
            } else {
                namebuf[nlen] = 0;
                findvar(namebuf, action);
                slot_to_vartbl[i] = g_VarIndex;
            }

            struct s_vartbl *v = &g_vartbl[slot_to_vartbl[i]];
            for (int d = 0; d < MAXDIM; d++) {
                v->dims[d] = (d < arr->ndims) ? arr->dims[d] : 0;
            }
        }

        int vi = slot_to_vartbl[i];
        struct s_vartbl *v = &g_vartbl[vi];

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
 * Sync MMBasic variable table -> VM globals (post-bridge-call).
 */
static void sync_mmbasic_to_vm(BCVMState *vm) {
    BCCompiler *cs = vm->compiler;
    if (!cs || !slot_map_initialized) return;

    for (uint16_t i = 0; i < cs->slot_count; i++) {
        BCSlot *slot = &cs->slots[i];
        if (!slot->name[0] || !isnamestart((unsigned char)slot->name[0])) continue;
        if (vm->arrays[i].data) continue;  /* arrays share data pointer */
        if (slot_to_vartbl[i] < 0) continue;

        struct s_vartbl *v = &g_vartbl[slot_to_vartbl[i]];

        if (slot->type == T_INT) {
            vm->globals[i].i = v->val.i;
        } else if (slot->type == T_NBR) {
            vm->globals[i].f = v->val.f;
        } else if (slot->type == T_STR) {
            if (v->val.s) {
                if (!vm->globals[i].s) {
                    vm->globals[i].s = BC_ALLOC(STRINGSIZE);
                }
                if (vm->globals[i].s) {
                    Mstrcpy(vm->globals[i].s, v->val.s);
                }
            }
        }
    }
}

/*
 * Sync VM locals -> MMBasic local variables (pre-bridge-call).
 * Returns the local scope level (for ClearVars cleanup).
 */
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

        int action = V_FIND | V_DIM_VAR | V_LOCAL | V_EMPTY_OK | meta->type;
        if (sf->return_type != 0 && i == 0) {
            action |= V_FUNCT;
        }

        if (meta->is_array) {
            BCArray *arr = &vm->local_arrays[slot];
            namebuf[nlen] = '(';
            namebuf[nlen + 1] = ')';
            namebuf[nlen + 2] = 0;
            findvar(namebuf, action);
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

        findvar(namebuf, action);
        local_map[slot] = g_VarIndex;
        struct s_vartbl *v = &g_vartbl[g_VarIndex];
        if (meta->type == T_INT) {
            v->val.i = vm->locals[slot].i;
        } else if (meta->type == T_NBR) {
            v->val.f = vm->locals[slot].f;
        } else if (meta->type == T_STR && vm->locals[slot].s) {
            v->val.s = GetTempMemory(STRINGSIZE);
            Mstrcpy(v->val.s, vm->locals[slot].s);
        }
    }

    return g_LocalIndex;
}

/*
 * Sync MMBasic locals -> VM locals (post-bridge-call).
 */
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

/* ------------------------------------------------------------------ */
/*  Bridge dispatch                                                    */
/* ------------------------------------------------------------------ */

/*
 * Decode a 2-byte command token to get the commandtbl index.
 */
static inline int bridge_decode_cmd(const uint8_t *p) {
    return (p[0] & 0x7f) | ((p[1] & 0x7f) << 7);
}

/*
 * bc_bridge_call_cmd — Execute a bridged command.
 *
 * tok points to the tokenized bytes (command token + args).
 * len is the total length of the tokenized data.
 *
 * The tokenized form starts with a 2-byte command token, followed by
 * the tokenized arguments, terminated by 0x00.
 */
void bc_bridge_call_cmd(BCVMState *vm, const uint8_t *tok, uint16_t len) {
    if (len < 2) return;

    /* Copy tokenized bytes to temp memory — command handlers may modify
     * the buffer during parsing. */
    unsigned char *buf = GetTempMemory(len + 1);
    memcpy(buf, tok, len);
    buf[len] = 0;

    int cmd_idx = bridge_decode_cmd(buf);

    if (cmd_idx < 0 || cmd_idx >= CommandTableSize) {
        bc_vm_error(vm, "Bridge: invalid command token %d", cmd_idx);
        return;
    }

    /* Save interpreter context */
    int saved_cmdtoken = cmdtoken;
    unsigned char *saved_cmdline = cmdline;
    unsigned char *saved_nextstmt = nextstmt;
    int saved_local_index = g_LocalIndex;
    int local_map[VM_MAX_LOCALS];
    int bridge_level = 0;

    /* Sync VM variables to MMBasic table */
    sync_vm_to_mmbasic(vm);
    bridge_level = sync_vm_locals_to_mmbasic(vm, local_map);

    /* Set up globals for the command handler */
    cmdtoken = cmd_idx;
    cmdline = buf + 2;  /* skip 2-byte command token */
    nextstmt = buf + len;

    /* Call the command handler */
    commandtbl[cmd_idx].fptr();

    /* Sync modified variables back to VM */
    sync_mmbasic_locals_to_vm(vm, local_map);
    sync_mmbasic_to_vm(vm);
    if (bridge_level) ClearVars(bridge_level, 1);
    g_LocalIndex = saved_local_index;

    /* Restore interpreter context */
    cmdtoken = saved_cmdtoken;
    cmdline = saved_cmdline;
    nextstmt = saved_nextstmt;

    ClearTempMemory();
}

/*
 * bc_bridge_call_fun — Execute a bridged function.
 *
 * fun_idx is the tokentbl index of the function.
 * args points to pre-tokenized argument bytes (what would go in ep).
 * arg_len is the length of the argument bytes (0 for T_FNA no-arg functions).
 * ret_type is the expected return type (T_INT, T_NBR, or T_STR).
 *
 * Sets up the interpreter's function-call globals (ep, targ) and calls
 * the function handler, then pushes the result onto the VM stack.
 */
void bc_bridge_call_fun(BCVMState *vm, uint16_t fun_idx, const uint8_t *args, uint16_t arg_len, uint8_t ret_type) {
    if (fun_idx >= (unsigned)TokenTableSize - 1) {
        bc_vm_error(vm, "Bridge: invalid function index %d", fun_idx);
        return;
    }

    /* Save interpreter context */
    int saved_targ = targ;
    unsigned char *saved_ep = ep;
    MMFLOAT saved_fret = fret;
    long long int saved_iret = iret;
    unsigned char *saved_sret = sret;
    int saved_local_index = g_LocalIndex;
    int local_map[VM_MAX_LOCALS];
    int bridge_level = 0;

    /* Sync VM variables to MMBasic table */
    sync_vm_to_mmbasic(vm);
    bridge_level = sync_vm_locals_to_mmbasic(vm, local_map);

    /* Set up function arguments in ep */
    if (arg_len > 0) {
        ep = GetTempMemory(STRINGSIZE);
        memcpy(ep, args, arg_len);
        ep[arg_len] = 0;
    } else {
        ep = GetTempMemory(1);
        ep[0] = 0;
    }

    targ = ret_type;
    tokentbl[fun_idx].fptr();

    /* Capture result before cleanup */
    MMFLOAT result_f = fret;
    long long int result_i = iret;
    unsigned char *result_s = sret;

    /* Sync modified variables back to VM */
    sync_mmbasic_locals_to_vm(vm, local_map);
    sync_mmbasic_to_vm(vm);
    if (bridge_level) ClearVars(bridge_level, 1);
    g_LocalIndex = saved_local_index;

    /* Push result onto VM stack */
    if (ret_type == T_INT) {
        if (vm->sp + 1 >= VM_STACK_SIZE) { bc_vm_error(vm, "Stack overflow"); goto cleanup; }
        vm->sp++;
        vm->stack[vm->sp].i = result_i;
        vm->stack_types[vm->sp] = T_INT;
    } else if (ret_type == T_NBR) {
        if (vm->sp + 1 >= VM_STACK_SIZE) { bc_vm_error(vm, "Stack overflow"); goto cleanup; }
        vm->sp++;
        vm->stack[vm->sp].f = result_f;
        vm->stack_types[vm->sp] = T_NBR;
    } else if (ret_type == T_STR) {
        /* Copy string to VM temp storage before ClearTempMemory */
        uint8_t *temp = &vm->str_temp[vm->str_temp_idx & 3][0];
        vm->str_temp_idx = (vm->str_temp_idx + 1) & 3;
        if (result_s) {
            int slen = *result_s;
            memcpy(temp, result_s, slen + 1);
        } else {
            temp[0] = 0;
        }
        if (vm->sp + 1 >= VM_STACK_SIZE) { bc_vm_error(vm, "Stack overflow"); goto cleanup; }
        vm->sp++;
        vm->stack[vm->sp].s = temp;
        vm->stack_types[vm->sp] = T_STR;
    }

cleanup:
    /* Restore interpreter context */
    targ = saved_targ;
    ep = saved_ep;
    fret = saved_fret;
    iret = saved_iret;
    sret = saved_sret;

    ClearTempMemory();
}
