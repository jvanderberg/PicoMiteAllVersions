/*
 * bc_vm.c - Bytecode VM for MMBasic's FRUN command
 *
 * Implements a stack-based virtual machine that executes the bytecode
 * produced by the FRUN compiler.  Uses computed goto (GCC extension)
 * for fast dispatch on RP2040.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "MMBasic.h"
#include "bytecode.h"
#ifndef MMBASIC_HOST
#include "pico/time.h"
#endif

/* Host: calloc/free (MMBasic heap uses 32-bit pointer math on 64-bit host) */
/* Device: GetMemory/FreeMemory (MMBasic heap) */
#ifdef MMBASIC_HOST
#define BC_ALLOC(sz)   calloc(1, (sz))
#define BC_FREE(p)     free((p))
#else
extern void *GetMemory(int size);
extern void FreeMemory(unsigned char *addr);
#define BC_ALLOC(sz)   GetMemory((sz))
#define BC_FREE(p)     FreeMemory((unsigned char *)(p))
#endif

extern void *GetTempMemory(int NbrBytes);
extern void *GetTempStrMemory(void);

/* External declarations */
extern int MMInkey(void);

/* Ensure __not_in_flash_func is defined (from pico SDK platform headers) */
#ifndef __not_in_flash_func
#define __not_in_flash_func(func) func
#endif

/* External MMBasic runtime functions */
extern void CheckAbort(void);
extern int  check_interrupt(void);

/* ======================================================================
 * Helper: get next rotating string temp buffer
 * ====================================================================== */
static uint8_t *vm_get_str_temp(BCVMState *vm) {
    uint8_t *p = vm->str_temp[vm->str_temp_idx];
    vm->str_temp_idx = (vm->str_temp_idx + 1) & 3;
    return p;
}

/* ======================================================================
 * Helper: calculate linear offset into an array (row-major)
 *
 * MMBasic arrays have dimension 0..N, so each dimension has size N+1.
 * ====================================================================== */
static uint32_t calc_array_offset(BCVMState *vm, BCArray *arr,
                                  int64_t *indices, int ndim) {
    uint32_t offset = 0;
    for (int d = 0; d < ndim; d++) {
        int dim_size = arr->dims[d] + 1;   /* 0..N inclusive = N+1 elements */
        if (indices[d] < 0 || indices[d] >= dim_size)
            bc_vm_error(vm, "Array index out of bounds");
        offset = offset * (uint32_t)dim_size + (uint32_t)indices[d];
    }
    if (offset >= arr->total_elements)
        bc_vm_error(vm, "Array index out of bounds");
    return offset;
}

/* ======================================================================
 * Helper: append to capture buffer or print to console
 * ====================================================================== */
static void vm_output(BCVMState *vm, const char *s) {
    if (vm->capture_buf) {
        int len = (int)strlen(s);
        /* Grow buffer if needed */
        while (vm->capture_len + len + 1 > vm->capture_cap) {
            int newcap = vm->capture_cap ? vm->capture_cap * 2 : 1024;
            char *nb = realloc(vm->capture_buf, newcap);
            if (!nb) bc_vm_error(vm, "Out of memory in capture buffer");
            vm->capture_buf = nb;
            vm->capture_cap = newcap;
        }
        memcpy(vm->capture_buf + vm->capture_len, s, len);
        vm->capture_len += len;
        vm->capture_buf[vm->capture_len] = '\0';
    } else {
        MMPrintString((char *)s);
    }
}

/* Output an MMBasic-format string (length-prefixed, not null-terminated) */
static void vm_output_mstr(BCVMState *vm, uint8_t *s) {
    if (!s) return;
    int len = s[0];
    if (len == 0) return;
    /* Build a null-terminated copy */
    char tmp[STRINGSIZE + 1];
    memcpy(tmp, s + 1, len);
    tmp[len] = '\0';
    vm_output(vm, tmp);
}

/* ======================================================================
 * bc_vm_alloc / bc_vm_free — dynamic allocation for large VM arrays
 * ====================================================================== */

int bc_vm_alloc(BCVMState *vm) {
    vm->globals      = (BCValue *)BC_ALLOC(BC_MAX_SLOTS * sizeof(BCValue));
    vm->global_types = (uint8_t *)BC_ALLOC(BC_MAX_SLOTS);
    vm->arrays       = (BCArray *)BC_ALLOC(BC_MAX_SLOTS * sizeof(BCArray));
    vm->locals       = (BCValue *)BC_ALLOC(VM_MAX_LOCALS * sizeof(BCValue));
    vm->local_types  = (uint8_t *)BC_ALLOC(VM_MAX_LOCALS);
    vm->local_arrays = (BCArray *)BC_ALLOC(VM_MAX_LOCALS * sizeof(BCArray));
    if (!vm->globals || !vm->global_types || !vm->arrays ||
        !vm->locals || !vm->local_types || !vm->local_arrays) {
        bc_vm_free(vm);
        return -1;
    }
    return 0;
}

void bc_vm_free(BCVMState *vm) {
    if (vm->globals)      BC_FREE(vm->globals);
    if (vm->global_types) BC_FREE(vm->global_types);
    if (vm->arrays)       BC_FREE(vm->arrays);
    if (vm->locals)       BC_FREE(vm->locals);
    if (vm->local_types)  BC_FREE(vm->local_types);
    if (vm->local_arrays) BC_FREE(vm->local_arrays);
    vm->globals = NULL;
    vm->global_types = NULL;
    vm->arrays = NULL;
    vm->locals = NULL;
    vm->local_types = NULL;
    vm->local_arrays = NULL;
}

/* ======================================================================
 * bc_vm_init — initialise VM state from compiled output
 *
 * Arrays must already be allocated via bc_vm_alloc().
 * ====================================================================== */
void bc_vm_init(BCVMState *vm, BCCompiler *cs) {
    /* Zero inline fields without touching the dynamic array pointers */
    memset(vm->stack, 0, sizeof(vm->stack));
    memset(vm->stack_types, 0, sizeof(vm->stack_types));
    memset(vm->call_stack, 0, sizeof(vm->call_stack));
    memset(vm->gosub_stack, 0, sizeof(vm->gosub_stack));
    memset(vm->for_stack, 0, sizeof(vm->for_stack));
    memset(vm->str_temp, 0, sizeof(vm->str_temp));

    /* Zero dynamically-allocated arrays.  On device GetMemory() zeros,
     * but on host calloc also zeros.  Keep explicit zeroing for safety. */
    if (vm->arrays)
        memset(vm->arrays, 0, BC_MAX_SLOTS * sizeof(BCArray));
    if (vm->local_arrays)
        memset(vm->local_arrays, 0, VM_MAX_LOCALS * sizeof(BCArray));
    if (vm->globals)
        memset(vm->globals, 0, BC_MAX_SLOTS * sizeof(BCValue));
    if (vm->global_types)
        memset(vm->global_types, 0, BC_MAX_SLOTS);
    if (vm->locals)
        memset(vm->locals, 0, VM_MAX_LOCALS * sizeof(BCValue));
    if (vm->local_types)
        memset(vm->local_types, 0, VM_MAX_LOCALS);
    vm->bytecode     = cs->code;
    vm->bytecode_len = cs->code_len;
    vm->compiler     = cs;
    vm->pc           = vm->bytecode;
    vm->sp           = -1;
    vm->csp          = 0;
    vm->gsp          = 0;
    vm->fsp          = 0;
    vm->frame_base   = 0;
    vm->locals_top   = 0;
    vm->capture_buf  = NULL;
    vm->capture_len  = 0;
    vm->capture_cap  = 0;
    vm->str_temp_idx = 0;
    vm->current_line = 0;

    /* Initialise global variable types from compiler slot table */
    for (int i = 0; i < cs->slot_count; i++) {
        vm->global_types[i] = cs->slots[i].type;
        /* Zero out the value */
        vm->globals[i].i = 0;
        /* For string slots, allocate a buffer */
        if ((cs->slots[i].type & (T_NBR | T_INT | T_STR)) == T_STR && !cs->slots[i].is_array) {
            /* Point to a zeroed temp; will be overwritten on first STORE_S */
        }
    }
}

/* ======================================================================
 * bc_vm_error — report error with current line number
 * ====================================================================== */
void bc_vm_error(BCVMState *vm, const char *msg, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, msg);
    vsnprintf(buf, sizeof(buf), msg, ap);
    va_end(ap);

    /* Dump VM state for debugging before the longjmp */
    bc_dump_vm_state(vm);

    /* Format with line number and call MMBasic's error() which longjmps */
    char errbuf[320];
    snprintf(errbuf, sizeof(errbuf), "[FRUN line %d] %s", (int)vm->current_line, buf);
    error(errbuf);
}

/* ======================================================================
 * bc_vm_execute — main dispatch loop using computed goto
 * ====================================================================== */
void __not_in_flash_func(bc_vm_execute)(BCVMState *vm) {

    /* ---- Helper macros ---- */
#define DISPATCH() goto *dispatch_table[*vm->pc++]

#define READ_U16() ({ uint16_t _v; memcpy(&_v, vm->pc, 2); vm->pc += 2; _v; })
#define READ_I16() ({ int16_t  _v; memcpy(&_v, vm->pc, 2); vm->pc += 2; _v; })
#define READ_U32() ({ uint32_t _v; memcpy(&_v, vm->pc, 4); vm->pc += 4; _v; })
#define READ_I64() ({ int64_t  _v; memcpy(&_v, vm->pc, 8); vm->pc += 8; _v; })
#define READ_F64() ({ MMFLOAT  _v; memcpy(&_v, vm->pc, 8); vm->pc += 8; _v; })

#define PUSH_I(val) do { \
    if (vm->sp >= VM_STACK_SIZE - 1) bc_vm_error(vm, "Stack overflow"); \
    vm->sp++; vm->stack[vm->sp].i = (val); vm->stack_types[vm->sp] = T_INT; \
} while(0)

#define PUSH_F(val) do { \
    if (vm->sp >= VM_STACK_SIZE - 1) bc_vm_error(vm, "Stack overflow"); \
    vm->sp++; vm->stack[vm->sp].f = (val); vm->stack_types[vm->sp] = T_NBR; \
} while(0)

#define PUSH_S(val) do { \
    if (vm->sp >= VM_STACK_SIZE - 1) bc_vm_error(vm, "Stack overflow"); \
    vm->sp++; vm->stack[vm->sp].s = (val); vm->stack_types[vm->sp] = T_STR; \
} while(0)

#define POP_I() (vm->stack[vm->sp--].i)
#define POP_F() (vm->stack[vm->sp--].f)
#define POP_S() (vm->stack[vm->sp--].s)
#define TOS_I() (vm->stack[vm->sp].i)
#define TOS_F() (vm->stack[vm->sp].f)

    /* ---- Build dispatch table ---- */
    static const void *dispatch_table[256] = {
        [0 ... 255] = &&op_invalid,

        /* Stack / Value Operations */
        [OP_NOP]            = &&op_nop,
        [OP_PUSH_INT]       = &&op_push_int,
        [OP_PUSH_FLT]       = &&op_push_flt,
        [OP_PUSH_STR]       = &&op_push_str,
        [OP_PUSH_ZERO]      = &&op_push_zero,
        [OP_PUSH_ONE]       = &&op_push_one,
        [OP_LOAD_I]         = &&op_load_i,
        [OP_LOAD_F]         = &&op_load_f,
        [OP_LOAD_S]         = &&op_load_s,
        [OP_STORE_I]        = &&op_store_i,
        [OP_STORE_F]        = &&op_store_f,
        [OP_STORE_S]        = &&op_store_s,
        [OP_LOAD_ARR_I]     = &&op_load_arr_i,
        [OP_LOAD_ARR_F]     = &&op_load_arr_f,
        [OP_LOAD_ARR_S]     = &&op_load_arr_s,
        [OP_STORE_ARR_I]    = &&op_store_arr_i,
        [OP_STORE_ARR_F]    = &&op_store_arr_f,
        [OP_STORE_ARR_S]    = &&op_store_arr_s,
        [OP_POP]            = &&op_pop,
        [OP_DUP]            = &&op_dup,
        [OP_CVT_I2F]        = &&op_cvt_i2f,
        [OP_CVT_F2I]        = &&op_cvt_f2i,

        /* Integer Arithmetic */
        [OP_ADD_I]          = &&op_add_i,
        [OP_SUB_I]          = &&op_sub_i,
        [OP_MUL_I]          = &&op_mul_i,
        [OP_IDIV_I]         = &&op_idiv_i,
        [OP_MOD_I]          = &&op_mod_i,

        /* Float Arithmetic */
        [OP_ADD_F]          = &&op_add_f,
        [OP_SUB_F]          = &&op_sub_f,
        [OP_MUL_F]          = &&op_mul_f,
        [OP_DIV_F]          = &&op_div_f,
        [OP_POW_F]          = &&op_pow_f,
        [OP_MOD_F]          = &&op_mod_f,

        /* String Operations */
        [OP_ADD_S]          = &&op_add_s,

        /* Unary */
        [OP_NEG_I]          = &&op_neg_i,
        [OP_NEG_F]          = &&op_neg_f,
        [OP_NOT]            = &&op_not,
        [OP_INV]            = &&op_inv,

        /* Bitwise / Logical */
        [OP_AND]            = &&op_and,
        [OP_OR]             = &&op_or,
        [OP_XOR]            = &&op_xor,
        [OP_SHL]            = &&op_shl,
        [OP_SHR]            = &&op_shr,

        /* Integer Comparison */
        [OP_EQ_I]           = &&op_eq_i,
        [OP_NE_I]           = &&op_ne_i,
        [OP_LT_I]           = &&op_lt_i,
        [OP_GT_I]           = &&op_gt_i,
        [OP_LE_I]           = &&op_le_i,
        [OP_GE_I]           = &&op_ge_i,

        /* Float Comparison */
        [OP_EQ_F]           = &&op_eq_f,
        [OP_NE_F]           = &&op_ne_f,
        [OP_LT_F]           = &&op_lt_f,
        [OP_GT_F]           = &&op_gt_f,
        [OP_LE_F]           = &&op_le_f,
        [OP_GE_F]           = &&op_ge_f,

        /* String Comparison */
        [OP_EQ_S]           = &&op_eq_s,
        [OP_NE_S]           = &&op_ne_s,
        [OP_LT_S]           = &&op_lt_s,
        [OP_GT_S]           = &&op_gt_s,
        [OP_LE_S]           = &&op_le_s,
        [OP_GE_S]           = &&op_ge_s,

        /* Control Flow */
        [OP_JMP]            = &&op_jmp,
        [OP_JMP_ABS]        = &&op_jmp_abs,
        [OP_JZ]             = &&op_jz,
        [OP_JNZ]            = &&op_jnz,
        [OP_GOSUB]          = &&op_gosub,
        [OP_RETURN]         = &&op_return,

        /* FOR loop */
        [OP_FOR_INIT_I]     = &&op_for_init_i,
        [OP_FOR_NEXT_I]     = &&op_for_next_i,
        [OP_FOR_INIT_F]     = &&op_for_init_f,
        [OP_FOR_NEXT_F]     = &&op_for_next_f,

        /* SUB / FUNCTION */
        [OP_CALL_SUB]       = &&op_call_sub,
        [OP_CALL_FUN]       = &&op_call_fun,
        [OP_RET_SUB]        = &&op_ret_sub,
        [OP_RET_FUN]        = &&op_ret_fun,
        [OP_ENTER_FRAME]    = &&op_enter_frame,
        [OP_LEAVE_FRAME]    = &&op_leave_frame,
        [OP_LOAD_LOCAL_I]   = &&op_load_local_i,
        [OP_LOAD_LOCAL_F]   = &&op_load_local_f,
        [OP_LOAD_LOCAL_S]   = &&op_load_local_s,
        [OP_STORE_LOCAL_I]  = &&op_store_local_i,
        [OP_STORE_LOCAL_F]  = &&op_store_local_f,
        [OP_STORE_LOCAL_S]  = &&op_store_local_s,
        [OP_LOAD_LOCAL_ARR_I]  = &&op_load_local_arr_i,
        [OP_LOAD_LOCAL_ARR_F]  = &&op_load_local_arr_f,
        [OP_LOAD_LOCAL_ARR_S]  = &&op_load_local_arr_s,
        [OP_STORE_LOCAL_ARR_I] = &&op_store_local_arr_i,

        /* Built-in Bridge */
        [OP_BUILTIN_CMD]    = &&op_builtin_cmd,
        [OP_BUILTIN_FUN_I]  = &&op_builtin_fun_i,
        [OP_BUILTIN_FUN_F]  = &&op_builtin_fun_f,
        [OP_BUILTIN_FUN_S]  = &&op_builtin_fun_s,

        /* PRINT */
        [OP_PRINT_INT]      = &&op_print_int,
        [OP_PRINT_FLT]      = &&op_print_flt,
        [OP_PRINT_STR]      = &&op_print_str,
        [OP_PRINT_NEWLINE]  = &&op_print_newline,
        [OP_PRINT_TAB]      = &&op_print_tab,

        /* DIM arrays */
        [OP_DIM_ARR_I]      = &&op_dim_arr_i,
        [OP_DIM_ARR_F]      = &&op_dim_arr_f,
        [OP_DIM_ARR_S]      = &&op_dim_arr_s,

        /* Native string functions */
        [OP_STR_LEN]        = &&op_str_len,
        [OP_STR_LEFT]       = &&op_str_left,
        [OP_STR_RIGHT]      = &&op_str_right,
        [OP_STR_MID2]       = &&op_str_mid2,
        [OP_STR_MID3]       = &&op_str_mid3,
        [OP_STR_UCASE]      = &&op_str_ucase,
        [OP_STR_LCASE]      = &&op_str_lcase,
        [OP_STR_VAL]        = &&op_str_val,
        [OP_STR_STR]        = &&op_str_str,
        [OP_STR_CHR]        = &&op_str_chr,
        [OP_STR_ASC]        = &&op_str_asc,
        [OP_STR_INSTR]      = &&op_str_instr,
        [OP_STR_HEX]        = &&op_str_hex,
        [OP_STR_OCT]        = &&op_str_oct,
        [OP_STR_BIN]        = &&op_str_bin,

        /* Native math functions */
        [OP_MATH_SIN]       = &&op_math_sin,
        [OP_MATH_COS]       = &&op_math_cos,
        [OP_MATH_TAN]       = &&op_math_tan,
        [OP_MATH_ATN]       = &&op_math_atn,
        [OP_MATH_SQR]       = &&op_math_sqr,
        [OP_MATH_LOG]       = &&op_math_log,
        [OP_MATH_EXP]       = &&op_math_exp,
        [OP_MATH_ABS]       = &&op_math_abs,
        [OP_MATH_SGN]       = &&op_math_sgn,
        [OP_MATH_INT]       = &&op_math_int,
        [OP_MATH_FIX]       = &&op_math_fix,
        [OP_MATH_CINT]      = &&op_math_cint,
        [OP_MATH_RAD]       = &&op_math_rad,
        [OP_MATH_DEG]       = &&op_math_deg,
        [OP_MATH_PI]        = &&op_math_pi,
        [OP_MATH_MAX]       = &&op_math_max,
        [OP_MATH_MIN]       = &&op_math_min,

        /* DATA / READ / RESTORE */
        [OP_READ_I]         = &&op_read_i,
        [OP_READ_F]         = &&op_read_f,
        [OP_READ_S]         = &&op_read_s,
        [OP_RESTORE]        = &&op_restore,

        /* Additional string functions */
        [OP_STR_SPACE]      = &&op_str_space,
        [OP_STR_STRING]     = &&op_str_string,
        [OP_STR_INKEY]      = &&op_str_inkey,

        /* Additional numeric functions */
        [OP_RND]            = &&op_rnd,

        /* Additional statements */
        [OP_RANDOMIZE]      = &&op_randomize,
        [OP_ERROR_S]        = &&op_error_s,
        [OP_ERROR_EMPTY]    = &&op_error_empty,
        [OP_CLEAR]          = &&op_clear,

        /* Housekeeping */
        [OP_LINE]           = &&op_line,
        [OP_CHECKINT]       = &&op_checkint,
        [OP_END]            = &&op_end,
        [OP_HALT]           = &&op_halt,
    };

    /* ---- Begin dispatch ---- */
    DISPATCH();

    /* ==================================================================
     * Stack / Value Operations
     * ================================================================== */

op_nop:
    DISPATCH();

op_push_int: {
    int64_t v = READ_I64();
    PUSH_I(v);
    DISPATCH();
}

op_push_flt: {
    MMFLOAT v = READ_F64();
    PUSH_F(v);
    DISPATCH();
}

op_push_str: {
    uint16_t idx = READ_U16();
    if (idx >= vm->compiler->const_count)
        bc_vm_error(vm, "Invalid string constant index %d", idx);
    BCConstant *c = &vm->compiler->constants[idx];
    /* Copy to a rotating temp buffer (MMBasic format: byte 0 = length) */
    uint8_t *tmp = vm_get_str_temp(vm);
    tmp[0] = (uint8_t)c->len;
    if (c->len > 0)
        memcpy(tmp + 1, c->data, c->len);
    PUSH_S(tmp);
    DISPATCH();
}

op_push_zero:
    PUSH_I(0);
    DISPATCH();

op_push_one:
    PUSH_I(1);
    DISPATCH();

op_load_i: {
    uint16_t slot = READ_U16();
    PUSH_I(vm->globals[slot].i);
    DISPATCH();
}

op_load_f: {
    uint16_t slot = READ_U16();
    PUSH_F(vm->globals[slot].f);
    DISPATCH();
}

op_load_s: {
    uint16_t slot = READ_U16();
    PUSH_S(vm->globals[slot].s);
    DISPATCH();
}

op_store_i: {
    uint16_t slot = READ_U16();
    if (vm->stack_types[vm->sp] == T_NBR)
        vm->globals[slot].i = (int64_t)POP_F();
    else
        vm->globals[slot].i = POP_I();
    DISPATCH();
}

op_store_f: {
    uint16_t slot = READ_U16();
    if (vm->stack_types[vm->sp] == T_INT)
        vm->globals[slot].f = (MMFLOAT)POP_I();
    else
        vm->globals[slot].f = POP_F();
    DISPATCH();
}

op_store_s: {
    uint16_t slot = READ_U16();
    uint8_t *src = POP_S();
    /* Allocate persistent storage for this global string if needed */
    if (!vm->globals[slot].s) {
        vm->globals[slot].s = GetTempMemory(STRINGSIZE);
    }
    Mstrcpy(vm->globals[slot].s, src);
    DISPATCH();
}

    /* ==================================================================
     * Array operations
     * ================================================================== */

op_load_arr_i: {
    uint16_t slot = READ_U16();
    uint8_t ndim = *vm->pc++;
    BCArray *arr = &vm->arrays[slot];
    if (!arr->data) bc_vm_error(vm, "Array not dimensioned");
    int64_t indices[MAXDIM];
    /* Indices are pushed first-dim-first, so pop in reverse */
    for (int d = ndim - 1; d >= 0; d--)
        indices[d] = POP_I();
    uint32_t off = calc_array_offset(vm, arr, indices, ndim);
    PUSH_I(arr->data[off].i);
    DISPATCH();
}

op_load_arr_f: {
    uint16_t slot = READ_U16();
    uint8_t ndim = *vm->pc++;
    BCArray *arr = &vm->arrays[slot];
    if (!arr->data) bc_vm_error(vm, "Array not dimensioned");
    int64_t indices[MAXDIM];
    for (int d = ndim - 1; d >= 0; d--)
        indices[d] = POP_I();
    uint32_t off = calc_array_offset(vm, arr, indices, ndim);
    PUSH_F(arr->data[off].f);
    DISPATCH();
}

op_load_arr_s: {
    uint16_t slot = READ_U16();
    uint8_t ndim = *vm->pc++;
    BCArray *arr = &vm->arrays[slot];
    if (!arr->data) bc_vm_error(vm, "Array not dimensioned");
    int64_t indices[MAXDIM];
    for (int d = ndim - 1; d >= 0; d--)
        indices[d] = POP_I();
    uint32_t off = calc_array_offset(vm, arr, indices, ndim);
    PUSH_S(arr->data[off].s);
    DISPATCH();
}

op_store_arr_i: {
    uint16_t slot = READ_U16();
    uint8_t ndim = *vm->pc++;
    BCArray *arr = &vm->arrays[slot];
    if (!arr->data) bc_vm_error(vm, "Array not dimensioned");
    int64_t val = POP_I();
    int64_t indices[MAXDIM];
    for (int d = ndim - 1; d >= 0; d--)
        indices[d] = POP_I();
    uint32_t off = calc_array_offset(vm, arr, indices, ndim);
    arr->data[off].i = val;
    DISPATCH();
}

op_store_arr_f: {
    uint16_t slot = READ_U16();
    uint8_t ndim = *vm->pc++;
    BCArray *arr = &vm->arrays[slot];
    if (!arr->data) bc_vm_error(vm, "Array not dimensioned");
    MMFLOAT val = POP_F();
    int64_t indices[MAXDIM];
    for (int d = ndim - 1; d >= 0; d--)
        indices[d] = POP_I();
    uint32_t off = calc_array_offset(vm, arr, indices, ndim);
    arr->data[off].f = val;
    DISPATCH();
}

op_store_arr_s: {
    uint16_t slot = READ_U16();
    uint8_t ndim = *vm->pc++;
    BCArray *arr = &vm->arrays[slot];
    if (!arr->data) bc_vm_error(vm, "Array not dimensioned");
    uint8_t *val = POP_S();
    int64_t indices[MAXDIM];
    for (int d = ndim - 1; d >= 0; d--)
        indices[d] = POP_I();
    uint32_t off = calc_array_offset(vm, arr, indices, ndim);
    /* Allocate string storage in the array element if needed */
    if (!arr->data[off].s) {
        arr->data[off].s = GetTempMemory(STRINGSIZE);
    }
    Mstrcpy(arr->data[off].s, val);
    DISPATCH();
}

op_pop:
    if (vm->sp < 0) bc_vm_error(vm, "Stack underflow");
    vm->sp--;
    DISPATCH();

op_dup: {
    if (vm->sp < 0) bc_vm_error(vm, "Stack underflow on DUP");
    if (vm->sp >= VM_STACK_SIZE - 1) bc_vm_error(vm, "Stack overflow on DUP");
    vm->stack[vm->sp + 1] = vm->stack[vm->sp];
    vm->stack_types[vm->sp + 1] = vm->stack_types[vm->sp];
    vm->sp++;
    DISPATCH();
}

op_cvt_i2f:
    vm->stack[vm->sp].f = (MMFLOAT)vm->stack[vm->sp].i;
    vm->stack_types[vm->sp] = T_NBR;
    DISPATCH();

op_cvt_f2i: {
    /* Match MMBasic FloatToInt64: round half away from zero */
    MMFLOAT x = vm->stack[vm->sp].f;
    vm->stack[vm->sp].i = (x >= 0) ? (int64_t)(x + 0.5) : (int64_t)(x - 0.5);
    vm->stack_types[vm->sp] = T_INT;
    DISPATCH();
}

    /* ==================================================================
     * Integer Arithmetic
     * ================================================================== */

op_add_i: {
    int64_t b = POP_I();
    vm->stack[vm->sp].i += b;
    DISPATCH();
}

op_sub_i: {
    int64_t b = POP_I();
    vm->stack[vm->sp].i -= b;
    DISPATCH();
}

op_mul_i: {
    int64_t b = POP_I();
    vm->stack[vm->sp].i *= b;
    DISPATCH();
}

op_idiv_i: {
    int64_t b = POP_I();
    if (b == 0) bc_vm_error(vm, "Division by zero");
    vm->stack[vm->sp].i /= b;
    DISPATCH();
}

op_mod_i: {
    int64_t b = POP_I();
    if (b == 0) bc_vm_error(vm, "Division by zero");
    vm->stack[vm->sp].i %= b;
    DISPATCH();
}

    /* ==================================================================
     * Float Arithmetic
     * ================================================================== */

op_add_f: {
    MMFLOAT b = POP_F();
    vm->stack[vm->sp].f += b;
    DISPATCH();
}

op_sub_f: {
    MMFLOAT b = POP_F();
    vm->stack[vm->sp].f -= b;
    DISPATCH();
}

op_mul_f: {
    MMFLOAT b = POP_F();
    vm->stack[vm->sp].f *= b;
    DISPATCH();
}

op_div_f: {
    MMFLOAT b = POP_F();
    if (b == 0.0) bc_vm_error(vm, "Division by zero");
    vm->stack[vm->sp].f /= b;
    DISPATCH();
}

op_pow_f: {
    MMFLOAT b = POP_F();
    vm->stack[vm->sp].f = pow(vm->stack[vm->sp].f, b);
    DISPATCH();
}

op_mod_f: {
    MMFLOAT b = POP_F();
    if (b == 0.0) bc_vm_error(vm, "Division by zero");
    vm->stack[vm->sp].f = fmod(vm->stack[vm->sp].f, b);
    DISPATCH();
}

    /* ==================================================================
     * String Concatenation
     * ================================================================== */

op_add_s: {
    /* MMBasic string format: byte[0] = length, bytes[1..len] = data */
    uint8_t *b = POP_S();
    uint8_t *a = POP_S();
    uint8_t *tmp = vm_get_str_temp(vm);
    int alen = a ? a[0] : 0;
    int blen = b ? b[0] : 0;
    int total = alen + blen;
    if (total > MAXSTRLEN)
        bc_vm_error(vm, "String too long");
    tmp[0] = (uint8_t)total;
    if (alen > 0) memcpy(tmp + 1, a + 1, alen);
    if (blen > 0) memcpy(tmp + 1 + alen, b + 1, blen);
    PUSH_S(tmp);
    DISPATCH();
}

    /* ==================================================================
     * Unary
     * ================================================================== */

op_neg_i:
    vm->stack[vm->sp].i = -vm->stack[vm->sp].i;
    DISPATCH();

op_neg_f:
    vm->stack[vm->sp].f = -vm->stack[vm->sp].f;
    DISPATCH();

op_not:
    /* MMBasic NOT is logical NOT (NOT 0 = 1, NOT nonzero = 0) */
    vm->stack[vm->sp].i = (vm->stack[vm->sp].i != 0) ? 0 : 1;
    DISPATCH();

op_inv:
    vm->stack[vm->sp].i = ~vm->stack[vm->sp].i;
    DISPATCH();

    /* ==================================================================
     * Bitwise / Logical
     * ================================================================== */

op_and: {
    int64_t b = POP_I();
    vm->stack[vm->sp].i &= b;
    DISPATCH();
}

op_or: {
    int64_t b = POP_I();
    vm->stack[vm->sp].i |= b;
    DISPATCH();
}

op_xor: {
    int64_t b = POP_I();
    vm->stack[vm->sp].i ^= b;
    DISPATCH();
}

op_shl: {
    int64_t b = POP_I();
    vm->stack[vm->sp].i <<= b;
    DISPATCH();
}

op_shr: {
    int64_t b = POP_I();
    /* Logical right shift via unsigned cast */
    vm->stack[vm->sp].i = (int64_t)((uint64_t)vm->stack[vm->sp].i >> b);
    DISPATCH();
}

    /* ==================================================================
     * Integer Comparison — produce int 0 or 1
     * ================================================================== */

op_eq_i: {
    int64_t b = POP_I();
    vm->stack[vm->sp].i = (vm->stack[vm->sp].i == b) ? 1 : 0;
    DISPATCH();
}

op_ne_i: {
    int64_t b = POP_I();
    vm->stack[vm->sp].i = (vm->stack[vm->sp].i != b) ? 1 : 0;
    DISPATCH();
}

op_lt_i: {
    int64_t b = POP_I();
    vm->stack[vm->sp].i = (vm->stack[vm->sp].i < b) ? 1 : 0;
    DISPATCH();
}

op_gt_i: {
    int64_t b = POP_I();
    vm->stack[vm->sp].i = (vm->stack[vm->sp].i > b) ? 1 : 0;
    DISPATCH();
}

op_le_i: {
    int64_t b = POP_I();
    vm->stack[vm->sp].i = (vm->stack[vm->sp].i <= b) ? 1 : 0;
    DISPATCH();
}

op_ge_i: {
    int64_t b = POP_I();
    vm->stack[vm->sp].i = (vm->stack[vm->sp].i >= b) ? 1 : 0;
    DISPATCH();
}

    /* ==================================================================
     * Float Comparison — produce int 0 or 1
     * ================================================================== */

op_eq_f: {
    MMFLOAT b = POP_F();
    MMFLOAT a = POP_F();
    PUSH_I((a == b) ? 1 : 0);
    DISPATCH();
}

op_ne_f: {
    MMFLOAT b = POP_F();
    MMFLOAT a = POP_F();
    PUSH_I((a != b) ? 1 : 0);
    DISPATCH();
}

op_lt_f: {
    MMFLOAT b = POP_F();
    MMFLOAT a = POP_F();
    PUSH_I((a < b) ? 1 : 0);
    DISPATCH();
}

op_gt_f: {
    MMFLOAT b = POP_F();
    MMFLOAT a = POP_F();
    PUSH_I((a > b) ? 1 : 0);
    DISPATCH();
}

op_le_f: {
    MMFLOAT b = POP_F();
    MMFLOAT a = POP_F();
    PUSH_I((a <= b) ? 1 : 0);
    DISPATCH();
}

op_ge_f: {
    MMFLOAT b = POP_F();
    MMFLOAT a = POP_F();
    PUSH_I((a >= b) ? 1 : 0);
    DISPATCH();
}

    /* ==================================================================
     * String Comparison — produce int 0 or 1
     *
     * Mstrcmp returns <0, 0, or >0 (like strcmp but for MMBasic strings).
     * ================================================================== */

op_eq_s: {
    uint8_t *b = POP_S();
    uint8_t *a = POP_S();
    PUSH_I(Mstrcmp(a, b) == 0 ? 1 : 0);
    DISPATCH();
}

op_ne_s: {
    uint8_t *b = POP_S();
    uint8_t *a = POP_S();
    PUSH_I(Mstrcmp(a, b) != 0 ? 1 : 0);
    DISPATCH();
}

op_lt_s: {
    uint8_t *b = POP_S();
    uint8_t *a = POP_S();
    PUSH_I(Mstrcmp(a, b) < 0 ? 1 : 0);
    DISPATCH();
}

op_gt_s: {
    uint8_t *b = POP_S();
    uint8_t *a = POP_S();
    PUSH_I(Mstrcmp(a, b) > 0 ? 1 : 0);
    DISPATCH();
}

op_le_s: {
    uint8_t *b = POP_S();
    uint8_t *a = POP_S();
    PUSH_I(Mstrcmp(a, b) <= 0 ? 1 : 0);
    DISPATCH();
}

op_ge_s: {
    uint8_t *b = POP_S();
    uint8_t *a = POP_S();
    PUSH_I(Mstrcmp(a, b) >= 0 ? 1 : 0);
    DISPATCH();
}

    /* ==================================================================
     * Control Flow
     * ================================================================== */

op_jmp: {
    int16_t offset = READ_I16();
    vm->pc += offset;
    DISPATCH();
}

op_jmp_abs: {
    uint32_t addr = READ_U32();
    vm->pc = vm->bytecode + addr;
    DISPATCH();
}

op_jz: {
    int16_t offset = READ_I16();
    int64_t val = POP_I();
    if (val == 0)
        vm->pc += offset;
    DISPATCH();
}

op_jnz: {
    int16_t offset = READ_I16();
    int64_t val = POP_I();
    if (val != 0)
        vm->pc += offset;
    DISPATCH();
}

op_gosub: {
    uint32_t addr = READ_U32();
    if (vm->gsp >= VM_MAX_GOSUB)
        bc_vm_error(vm, "GOSUB stack overflow");
    vm->gosub_stack[vm->gsp].return_pc = vm->pc;
    vm->gsp++;
    vm->pc = vm->bytecode + addr;
    DISPATCH();
}

op_return: {
    if (vm->gsp <= 0)
        bc_vm_error(vm, "RETURN without GOSUB");
    vm->gsp--;
    vm->pc = vm->gosub_stack[vm->gsp].return_pc;
    DISPATCH();
}

    /* ==================================================================
     * FOR Loop — Integer
     * ================================================================== */

op_for_init_i: {
    uint16_t raw_var   = READ_U16();
    uint16_t lim_slot  = READ_U16();
    uint16_t step_slot = READ_U16();
    int16_t  exit_off  = READ_I16();

    int var_is_local = (raw_var & 0x8000) != 0;
    uint16_t var_slot = raw_var & 0x7FFF;

    if (vm->fsp >= VM_MAX_FOR)
        bc_vm_error(vm, "FOR stack overflow");

    /* Push for-stack entry */
    BCForEntry *fe = &vm->for_stack[vm->fsp];
    fe->var_slot  = raw_var;  /* preserve flag for NEXT */
    fe->lim_slot  = lim_slot;
    fe->step_slot = step_slot;
    fe->loop_top  = vm->pc;       /* loop body starts here */
    fe->is_local  = var_is_local;
    fe->var_type  = T_INT;
    vm->fsp++;

    /* Check if already past limit */
    BCValue *var_ptr = var_is_local ? &vm->locals[vm->frame_base + var_slot] : &vm->globals[var_slot];
    int64_t val  = var_ptr->i;
    int64_t lim  = vm->globals[lim_slot].i;
    int64_t step = vm->globals[step_slot].i;
    if (step > 0 && val > lim) {
        vm->fsp--;
        vm->pc += exit_off;
    } else if (step < 0 && val < lim) {
        vm->fsp--;
        vm->pc += exit_off;
    } else if (step == 0) {
        bc_vm_error(vm, "FOR step cannot be zero");
    }
    DISPATCH();
}

op_for_next_i: {
    uint16_t raw_var   = READ_U16();
    uint16_t lim_slot  = READ_U16();
    uint16_t step_slot = READ_U16();
    int16_t  loop_off  = READ_I16();

    int var_is_local = (raw_var & 0x8000) != 0;
    uint16_t var_slot = raw_var & 0x7FFF;
    BCValue *var_ptr = var_is_local ? &vm->locals[vm->frame_base + var_slot] : &vm->globals[var_slot];

    int64_t step = vm->globals[step_slot].i;
    var_ptr->i += step;

    int64_t val = var_ptr->i;
    int64_t lim = vm->globals[lim_slot].i;

    int past;
    if (step > 0)
        past = (val > lim);
    else
        past = (val < lim);

    if (!past) {
        vm->pc += loop_off;
    } else {
        /* Loop done, pop for-stack */
        if (vm->fsp > 0) vm->fsp--;
    }
    DISPATCH();
}

    /* ==================================================================
     * FOR Loop — Float
     * ================================================================== */

op_for_init_f: {
    uint16_t raw_var   = READ_U16();
    uint16_t lim_slot  = READ_U16();
    uint16_t step_slot = READ_U16();
    int16_t  exit_off  = READ_I16();

    int var_is_local = (raw_var & 0x8000) != 0;
    uint16_t var_slot = raw_var & 0x7FFF;

    if (vm->fsp >= VM_MAX_FOR)
        bc_vm_error(vm, "FOR stack overflow");

    BCForEntry *fe = &vm->for_stack[vm->fsp];
    fe->var_slot  = raw_var;  /* preserve flag for NEXT */
    fe->lim_slot  = lim_slot;
    fe->step_slot = step_slot;
    fe->loop_top  = vm->pc;
    fe->is_local  = var_is_local;
    fe->var_type  = T_NBR;
    vm->fsp++;

    BCValue *var_ptr = var_is_local ? &vm->locals[vm->frame_base + var_slot] : &vm->globals[var_slot];
    MMFLOAT val  = var_ptr->f;
    MMFLOAT lim  = vm->globals[lim_slot].f;
    MMFLOAT step = vm->globals[step_slot].f;
    if (step > 0.0 && val > lim) {
        vm->fsp--;
        vm->pc += exit_off;
    } else if (step < 0.0 && val < lim) {
        vm->fsp--;
        vm->pc += exit_off;
    } else if (step == 0.0) {
        bc_vm_error(vm, "FOR step cannot be zero");
    }
    DISPATCH();
}

op_for_next_f: {
    uint16_t raw_var   = READ_U16();
    uint16_t lim_slot  = READ_U16();
    uint16_t step_slot = READ_U16();
    int16_t  loop_off  = READ_I16();

    int var_is_local = (raw_var & 0x8000) != 0;
    uint16_t var_slot = raw_var & 0x7FFF;
    BCValue *var_ptr = var_is_local ? &vm->locals[vm->frame_base + var_slot] : &vm->globals[var_slot];

    MMFLOAT step = vm->globals[step_slot].f;
    var_ptr->f += step;

    MMFLOAT val = var_ptr->f;
    MMFLOAT lim = vm->globals[lim_slot].f;

    int past;
    if (step > 0.0)
        past = (val > lim);
    else
        past = (val < lim);

    if (!past) {
        vm->pc += loop_off;
    } else {
        if (vm->fsp > 0) vm->fsp--;
    }
    DISPATCH();
}

    /* ==================================================================
     * SUB / FUNCTION calls
     * ================================================================== */

op_call_sub: {
    uint16_t idx   = READ_U16();
    uint8_t  nargs = *vm->pc++;

    if (idx >= vm->compiler->subfun_count)
        bc_vm_error(vm, "Invalid SUB index %d", idx);

    if (vm->csp >= VM_MAX_CALL)
        bc_vm_error(vm, "Call stack overflow");

    BCSubFun *sf = &vm->compiler->subfuns[idx];

    /* Push call frame */
    BCCallFrame *cf = &vm->call_stack[vm->csp];
    cf->return_pc  = vm->pc;
    cf->frame_base = vm->frame_base;
    cf->saved_sp   = vm->sp - nargs;  /* SP after popping args */
    cf->nlocals    = sf->nlocals;
    vm->csp++;

    /* Set new frame base */
    int new_base = vm->locals_top;
    vm->frame_base = new_base;

    /* Pop arguments into local slots (they are pushed left-to-right,
       so the first arg is deepest on the stack) */
    for (int i = nargs - 1; i >= 0; i--) {
        if (new_base + i >= VM_MAX_LOCALS)
            bc_vm_error(vm, "Local variable overflow");
        vm->locals[new_base + i] = vm->stack[vm->sp];
        vm->local_types[new_base + i] = vm->stack_types[vm->sp];
        /* Deep-copy strings so they survive temp buffer rotation */
        if (vm->stack_types[vm->sp] == T_STR && vm->stack[vm->sp].s) {
            uint8_t *copy = GetTempMemory(STRINGSIZE);
            Mstrcpy(copy, vm->stack[vm->sp].s);
            vm->locals[new_base + i].s = copy;
        }
        vm->sp--;
    }

    /* Update locals_top so ENTER_FRAME won't zero the args */
    vm->locals_top = new_base + nargs;

    /* Jump to entry point */
    vm->pc = vm->bytecode + sf->entry_addr;
    DISPATCH();
}

op_call_fun: {
    uint16_t idx   = READ_U16();
    uint8_t  nargs = *vm->pc++;

    if (idx >= vm->compiler->subfun_count)
        bc_vm_error(vm, "Invalid FUNCTION index %d", idx);

    if (vm->csp >= VM_MAX_CALL)
        bc_vm_error(vm, "Call stack overflow");

    BCSubFun *sf = &vm->compiler->subfuns[idx];

    /* Push call frame */
    BCCallFrame *cf = &vm->call_stack[vm->csp];
    cf->return_pc  = vm->pc;
    cf->frame_base = vm->frame_base;
    cf->saved_sp   = vm->sp - nargs;  /* Where SP should be after popping args (return val goes here+1) */
    cf->nlocals    = sf->nlocals;
    vm->csp++;

    int new_base = vm->locals_top;
    vm->frame_base = new_base;

    /* For FUNCTIONs, slot 0 is the return value — args go to slots 1..nargs */
    for (int i = nargs - 1; i >= 0; i--) {
        int slot = new_base + 1 + i;
        if (slot >= VM_MAX_LOCALS)
            bc_vm_error(vm, "Local variable overflow");
        vm->locals[slot] = vm->stack[vm->sp];
        vm->local_types[slot] = vm->stack_types[vm->sp];
        /* Deep-copy strings so they survive temp buffer rotation */
        if (vm->stack_types[vm->sp] == T_STR && vm->stack[vm->sp].s) {
            uint8_t *copy = GetTempMemory(STRINGSIZE);
            Mstrcpy(copy, vm->stack[vm->sp].s);
            vm->locals[slot].s = copy;
        }
        vm->sp--;
    }

    /* Zero out slot 0 (return value) */
    vm->locals[new_base].i = 0;
    vm->local_types[new_base] = sf->return_type;

    /* Update locals_top so ENTER_FRAME won't zero the args */
    vm->locals_top = new_base + 1 + nargs;

    vm->pc = vm->bytecode + sf->entry_addr;
    DISPATCH();
}

op_enter_frame: {
    uint16_t nlocals = READ_U16();
    /* Reserve nlocals slots in the locals array.  The first N may already
       be populated by arguments (from CALL_SUB/CALL_FUN).  The rest are
       zeroed for LOCAL variables. */
    if (vm->locals_top + nlocals > VM_MAX_LOCALS)
        bc_vm_error(vm, "Local variable overflow in ENTER_FRAME");
    /* Zero out local slots beyond what args may have set */
    for (int i = 0; i < nlocals; i++) {
        int idx = vm->frame_base + i;
        /* Only zero if this slot wasn't set by args.  However, since we
           always write args before ENTER_FRAME runs, we can safely
           just zero-init everything beyond the arg range.  ENTER_FRAME
           should reflect total locals including params. The args were
           already placed. */
        if (idx >= vm->locals_top) {
            vm->locals[idx].i = 0;
            vm->local_types[idx] = T_INT;
        }
    }
    vm->locals_top = vm->frame_base + nlocals;
    DISPATCH();
}

op_leave_frame: {
    /* Free any local string memory.  For simplicity, we just release
       the slots — string memory was allocated via GetTempMemory which
       is freed in bulk when the program ends. */
    if (vm->csp > 0) {
        BCCallFrame *cf = &vm->call_stack[vm->csp - 1];
        vm->locals_top = vm->frame_base;
        /* local_arrays cleanup: zero out local array data pointers */
        for (int i = vm->frame_base; i < vm->frame_base + (int)cf->nlocals; i++) {
            if (i < VM_MAX_LOCALS && vm->local_arrays[i].data) {
                BC_FREE(vm->local_arrays[i].data);
                vm->local_arrays[i].data = NULL;
                vm->local_arrays[i].total_elements = 0;
            }
        }
    }
    DISPATCH();
}

op_ret_sub: {
    if (vm->csp <= 0)
        bc_vm_error(vm, "RETURN SUB without matching CALL");
    vm->csp--;
    BCCallFrame *cf = &vm->call_stack[vm->csp];
    vm->frame_base = cf->frame_base;
    vm->locals_top = vm->frame_base;  /* Reclaim local slots */
    vm->sp = cf->saved_sp;
    vm->pc = cf->return_pc;
    DISPATCH();
}

op_ret_fun: {
    if (vm->csp <= 0)
        bc_vm_error(vm, "RETURN FUN without matching CALL");
    /* Return value is on TOS — save it */
    BCValue ret_val = vm->stack[vm->sp];
    uint8_t ret_type = vm->stack_types[vm->sp];

    vm->csp--;
    BCCallFrame *cf = &vm->call_stack[vm->csp];
    vm->frame_base = cf->frame_base;
    vm->locals_top = vm->frame_base;
    vm->sp = cf->saved_sp;
    vm->pc = cf->return_pc;

    /* Push return value onto the caller's stack */
    vm->sp++;
    vm->stack[vm->sp] = ret_val;
    vm->stack_types[vm->sp] = ret_type;
    DISPATCH();
}

    /* ==================================================================
     * Local variable access
     * ================================================================== */

op_load_local_i: {
    uint16_t offset = READ_U16();
    int idx = vm->frame_base + offset;
    if (idx >= VM_MAX_LOCALS) bc_vm_error(vm, "Local index out of range");
    PUSH_I(vm->locals[idx].i);
    DISPATCH();
}

op_load_local_f: {
    uint16_t offset = READ_U16();
    int idx = vm->frame_base + offset;
    if (idx >= VM_MAX_LOCALS) bc_vm_error(vm, "Local index out of range");
    PUSH_F(vm->locals[idx].f);
    DISPATCH();
}

op_load_local_s: {
    uint16_t offset = READ_U16();
    int idx = vm->frame_base + offset;
    if (idx >= VM_MAX_LOCALS) bc_vm_error(vm, "Local index out of range");
    PUSH_S(vm->locals[idx].s);
    DISPATCH();
}

op_store_local_i: {
    uint16_t offset = READ_U16();
    int idx = vm->frame_base + offset;
    if (idx >= VM_MAX_LOCALS) bc_vm_error(vm, "Local index out of range");
    if (vm->stack_types[vm->sp] == T_NBR)
        vm->locals[idx].i = (int64_t)POP_F();
    else
        vm->locals[idx].i = POP_I();
    vm->local_types[idx] = T_INT;
    DISPATCH();
}

op_store_local_f: {
    uint16_t offset = READ_U16();
    int idx = vm->frame_base + offset;
    if (idx >= VM_MAX_LOCALS) bc_vm_error(vm, "Local index out of range");
    if (vm->stack_types[vm->sp] == T_INT)
        vm->locals[idx].f = (MMFLOAT)POP_I();
    else
        vm->locals[idx].f = POP_F();
    vm->local_types[idx] = T_NBR;
    DISPATCH();
}

op_store_local_s: {
    uint16_t offset = READ_U16();
    int idx = vm->frame_base + offset;
    if (idx >= VM_MAX_LOCALS) bc_vm_error(vm, "Local index out of range");
    uint8_t *src = POP_S();
    if (!vm->locals[idx].s) {
        vm->locals[idx].s = GetTempMemory(STRINGSIZE);
    }
    Mstrcpy(vm->locals[idx].s, src);
    vm->local_types[idx] = T_STR;
    DISPATCH();
}

    /* ==================================================================
     * Local array access
     * ================================================================== */

op_load_local_arr_i: {
    uint16_t offset = READ_U16();
    uint8_t ndim = *vm->pc++;
    int idx = vm->frame_base + offset;
    if (idx >= VM_MAX_LOCALS) bc_vm_error(vm, "Local array index out of range");
    BCArray *arr = &vm->local_arrays[idx];
    if (!arr->data) bc_vm_error(vm, "Local array not dimensioned");
    int64_t indices[MAXDIM];
    for (int d = ndim - 1; d >= 0; d--)
        indices[d] = POP_I();
    uint32_t off = calc_array_offset(vm, arr, indices, ndim);
    PUSH_I(arr->data[off].i);
    DISPATCH();
}

op_load_local_arr_f: {
    uint16_t offset = READ_U16();
    uint8_t ndim = *vm->pc++;
    int idx = vm->frame_base + offset;
    if (idx >= VM_MAX_LOCALS) bc_vm_error(vm, "Local array index out of range");
    BCArray *arr = &vm->local_arrays[idx];
    if (!arr->data) bc_vm_error(vm, "Local array not dimensioned");
    int64_t indices[MAXDIM];
    for (int d = ndim - 1; d >= 0; d--)
        indices[d] = POP_I();
    uint32_t off = calc_array_offset(vm, arr, indices, ndim);
    PUSH_F(arr->data[off].f);
    DISPATCH();
}

op_load_local_arr_s: {
    uint16_t offset = READ_U16();
    uint8_t ndim = *vm->pc++;
    int idx = vm->frame_base + offset;
    if (idx >= VM_MAX_LOCALS) bc_vm_error(vm, "Local array index out of range");
    BCArray *arr = &vm->local_arrays[idx];
    if (!arr->data) bc_vm_error(vm, "Local array not dimensioned");
    int64_t indices[MAXDIM];
    for (int d = ndim - 1; d >= 0; d--)
        indices[d] = POP_I();
    uint32_t off = calc_array_offset(vm, arr, indices, ndim);
    PUSH_S(arr->data[off].s);
    DISPATCH();
}

op_store_local_arr_i: {
    uint16_t offset = READ_U16();
    uint8_t ndim = *vm->pc++;
    int idx = vm->frame_base + offset;
    if (idx >= VM_MAX_LOCALS) bc_vm_error(vm, "Local array index out of range");
    BCArray *arr = &vm->local_arrays[idx];
    if (!arr->data) bc_vm_error(vm, "Local array not dimensioned");
    int64_t val = POP_I();
    int64_t indices[MAXDIM];
    for (int d = ndim - 1; d >= 0; d--)
        indices[d] = POP_I();
    uint32_t off = calc_array_offset(vm, arr, indices, ndim);
    arr->data[off].i = val;
    DISPATCH();
}

    /* ==================================================================
     * Built-in Bridge
     * ================================================================== */

op_builtin_cmd: {
    uint16_t idx = READ_U16();
    bc_bridge_call_cmd(vm, idx);
    DISPATCH();
}

op_builtin_fun_i: {
    uint16_t idx   = READ_U16();
    uint8_t  nargs = *vm->pc++;
    bc_bridge_call_fun(vm, idx, nargs, T_INT);
    DISPATCH();
}

op_builtin_fun_f: {
    uint16_t idx   = READ_U16();
    uint8_t  nargs = *vm->pc++;
    bc_bridge_call_fun(vm, idx, nargs, T_NBR);
    DISPATCH();
}

op_builtin_fun_s: {
    uint16_t idx   = READ_U16();
    uint8_t  nargs = *vm->pc++;
    bc_bridge_call_fun(vm, idx, nargs, T_STR);
    DISPATCH();
}

    /* ==================================================================
     * PRINT
     *
     * MMBasic PRINT behaviour:
     *   - Positive numbers get a leading space (in place of the + sign)
     *   - Numbers get a trailing space
     *   - PRINT_SEMICOLON suppresses the trailing newline / separator
     * ================================================================== */

op_print_int: {
    uint8_t flags = *vm->pc++;
    (void)flags;
    int64_t val = POP_I();
    char buf[64];
    IntToStr(buf, val, 10);
    /* MMBasic: positive numbers get leading space, no trailing space */
    char out[80];
    int pos = 0;
    if (val >= 0) out[pos++] = ' ';
    int blen = (int)strlen(buf);
    memcpy(out + pos, buf, blen);
    pos += blen;
    out[pos] = '\0';
    vm_output(vm, out);
    DISPATCH();
}

op_print_flt: {
    uint8_t flags = *vm->pc++;
    (void)flags;
    MMFLOAT val = POP_F();
    char buf[64];
    FloatToStr(buf, val, 0, STR_AUTO_PRECISION, ' ');
    /* MMBasic: positive floats get leading space, no trailing space */
    char out[80];
    int pos = 0;
    if (val >= 0.0) out[pos++] = ' ';
    int blen = (int)strlen(buf);
    /* FloatToStr may already include leading space — skip it */
    int start = 0;
    while (start < blen && buf[start] == ' ') start++;
    memcpy(out + pos, buf + start, blen - start);
    pos += blen - start;
    out[pos] = '\0';
    vm_output(vm, out);
    DISPATCH();
}

op_print_str: {
    uint8_t flags = *vm->pc++;
    uint8_t *val = POP_S();
    (void)flags;
    vm_output_mstr(vm, val);
    DISPATCH();
}

op_print_newline:
    vm_output(vm, "\r\n");
    DISPATCH();

op_print_tab:
    vm_output(vm, "\t");
    DISPATCH();

    /* ==================================================================
     * DIM arrays
     * ================================================================== */

op_dim_arr_i: {
    uint16_t slot = READ_U16();
    uint8_t ndim = *vm->pc++;
    BCArray *arr = &vm->arrays[slot];
    if (arr->data) bc_vm_error(vm, "Array already dimensioned");
    arr->ndims = ndim;
    arr->elem_type = T_INT;
    uint32_t total = 1;
    int64_t dims[MAXDIM];
    /* Pop dimension sizes (last dim first) */
    for (int d = ndim - 1; d >= 0; d--)
        dims[d] = POP_I();
    for (int d = 0; d < ndim; d++) {
        if (dims[d] < 0) bc_vm_error(vm, "Invalid array dimension");
        arr->dims[d] = (int)dims[d];
        total *= (uint32_t)(dims[d] + 1);  /* 0..N inclusive */
    }
    arr->total_elements = total;
    arr->data = (BCValue *)BC_ALLOC(total * sizeof(BCValue));
    if (!arr->data) bc_vm_error(vm, "Out of memory for array");
    memset(arr->data, 0, total * sizeof(BCValue));
    DISPATCH();
}

op_dim_arr_f: {
    uint16_t slot = READ_U16();
    uint8_t ndim = *vm->pc++;
    BCArray *arr = &vm->arrays[slot];
    if (arr->data) bc_vm_error(vm, "Array already dimensioned");
    arr->ndims = ndim;
    arr->elem_type = T_NBR;
    uint32_t total = 1;
    int64_t dims[MAXDIM];
    for (int d = ndim - 1; d >= 0; d--)
        dims[d] = POP_I();
    for (int d = 0; d < ndim; d++) {
        if (dims[d] < 0) bc_vm_error(vm, "Invalid array dimension");
        arr->dims[d] = (int)dims[d];
        total *= (uint32_t)(dims[d] + 1);
    }
    arr->total_elements = total;
    arr->data = (BCValue *)BC_ALLOC(total * sizeof(BCValue));
    if (!arr->data) bc_vm_error(vm, "Out of memory for array");
    memset(arr->data, 0, total * sizeof(BCValue));
    DISPATCH();
}

op_dim_arr_s: {
    uint16_t slot = READ_U16();
    uint8_t ndim = *vm->pc++;
    BCArray *arr = &vm->arrays[slot];
    if (arr->data) bc_vm_error(vm, "Array already dimensioned");
    arr->ndims = ndim;
    arr->elem_type = T_STR;
    uint32_t total = 1;
    int64_t dims[MAXDIM];
    for (int d = ndim - 1; d >= 0; d--)
        dims[d] = POP_I();
    for (int d = 0; d < ndim; d++) {
        if (dims[d] < 0) bc_vm_error(vm, "Invalid array dimension");
        arr->dims[d] = (int)dims[d];
        total *= (uint32_t)(dims[d] + 1);
    }
    arr->total_elements = total;
    arr->data = (BCValue *)BC_ALLOC(total * sizeof(BCValue));
    if (!arr->data) bc_vm_error(vm, "Out of memory for array");
    /* Allocate string buffers for each element */
    for (uint32_t i = 0; i < total; i++) {
        arr->data[i].s = GetTempMemory(STRINGSIZE);
        arr->data[i].s[0] = 0;  /* empty string (length prefix = 0) */
    }
    DISPATCH();
}

    /* ==================================================================
     * Native String Functions
     * ================================================================== */

/* Helper: get a rotating temp string buffer */
#define STR_TEMP() vm_get_str_temp(vm)

op_str_len: {
    /* LEN(str$) -> int */
    uint8_t *s = POP_S();
    PUSH_I((int64_t)(s ? s[0] : 0));
    DISPATCH();
}

op_str_left: {
    /* LEFT$(str$, n) -> str$ */
    int64_t n = POP_I();
    uint8_t *s = POP_S();
    uint8_t *temp = STR_TEMP();
    int slen = s ? s[0] : 0;
    if (n < 0) n = 0;
    if (n > slen) n = slen;
    temp[0] = (uint8_t)n;
    if (n > 0) memcpy(&temp[1], &s[1], n);
    PUSH_S(temp);
    DISPATCH();
}

op_str_right: {
    /* RIGHT$(str$, n) -> str$ */
    int64_t n = POP_I();
    uint8_t *s = POP_S();
    uint8_t *temp = STR_TEMP();
    int slen = s ? s[0] : 0;
    if (n < 0) n = 0;
    if (n > slen) n = slen;
    temp[0] = (uint8_t)n;
    if (n > 0) memcpy(&temp[1], &s[1 + slen - (int)n], n);
    PUSH_S(temp);
    DISPATCH();
}

op_str_mid2: {
    /* MID$(str$, start) -> str$ (from start to end) */
    int64_t start = POP_I();
    uint8_t *s = POP_S();
    uint8_t *temp = STR_TEMP();
    int slen = s ? s[0] : 0;
    if (start < 1) start = 1;
    if (start > slen) { temp[0] = 0; PUSH_S(temp); DISPATCH(); }
    int n = slen - (int)start + 1;
    temp[0] = (uint8_t)n;
    memcpy(&temp[1], &s[(int)start], n);
    PUSH_S(temp);
    DISPATCH();
}

op_str_mid3: {
    /* MID$(str$, start, len) -> str$ */
    int64_t len = POP_I();
    int64_t start = POP_I();
    uint8_t *s = POP_S();
    uint8_t *temp = STR_TEMP();
    int slen = s ? s[0] : 0;
    if (start < 1) start = 1;
    if (start > slen || len <= 0) { temp[0] = 0; PUSH_S(temp); DISPATCH(); }
    int avail = slen - (int)start + 1;
    if (len > avail) len = avail;
    temp[0] = (uint8_t)len;
    memcpy(&temp[1], &s[(int)start], (int)len);
    PUSH_S(temp);
    DISPATCH();
}

op_str_ucase: {
    /* UCASE$(str$) -> str$ */
    uint8_t *s = POP_S();
    uint8_t *temp = STR_TEMP();
    int slen = s ? s[0] : 0;
    temp[0] = (uint8_t)slen;
    for (int i = 0; i < slen; i++)
        temp[1 + i] = toupper(s[1 + i]);
    PUSH_S(temp);
    DISPATCH();
}

op_str_lcase: {
    /* LCASE$(str$) -> str$ */
    uint8_t *s = POP_S();
    uint8_t *temp = STR_TEMP();
    int slen = s ? s[0] : 0;
    temp[0] = (uint8_t)slen;
    for (int i = 0; i < slen; i++)
        temp[1 + i] = tolower(s[1 + i]);
    PUSH_S(temp);
    DISPATCH();
}

    /* ==================================================================
     * Additional Native String Functions
     * ================================================================== */

op_str_val: {
    /* VAL(str$) -> float */
    uint8_t *s = POP_S();
    if (!s || s[0] == 0) { PUSH_F(0.0); DISPATCH(); }
    char tmp[STRINGSIZE + 1];
    int slen = s[0];
    memcpy(tmp, s + 1, slen);
    tmp[slen] = '\0';
    MMFLOAT v = (MMFLOAT)strtod(tmp, NULL);
    PUSH_F(v);
    DISPATCH();
}

op_str_str: {
    /* STR$(n) -> str$ */
    /* The argument could be int or float — check stack type */
    uint8_t *temp = STR_TEMP();
    if (vm->stack_types[vm->sp] == T_INT) {
        int64_t v = POP_I();
        char buf[64];
        IntToStr(buf, v, 10);
        int blen = (int)strlen(buf);
        temp[0] = (uint8_t)blen;
        memcpy(temp + 1, buf, blen);
    } else {
        MMFLOAT v = POP_F();
        char buf[64];
        FloatToStr(buf, v, 0, STR_AUTO_PRECISION, ' ');
        /* Strip leading spaces */
        char *bp = buf;
        while (*bp == ' ') bp++;
        int blen = (int)strlen(bp);
        temp[0] = (uint8_t)blen;
        memcpy(temp + 1, bp, blen);
    }
    PUSH_S(temp);
    DISPATCH();
}

op_str_chr: {
    /* CHR$(n%) -> str$ */
    int64_t n = POP_I();
    uint8_t *temp = STR_TEMP();
    temp[0] = 1;
    temp[1] = (uint8_t)(n & 0xFF);
    PUSH_S(temp);
    DISPATCH();
}

op_str_asc: {
    /* ASC(str$) -> int */
    uint8_t *s = POP_S();
    if (!s || s[0] == 0) { PUSH_I(0); DISPATCH(); }
    PUSH_I((int64_t)s[1]);
    DISPATCH();
}

op_str_instr: {
    /* INSTR([start%,] haystack$, needle$) -> int (1-based, 0 if not found)
     * Followed by 1-byte arg count (2 or 3). */
    uint8_t nargs = *vm->pc++;
    uint8_t *needle = POP_S();
    uint8_t *haystack = POP_S();
    int start = 0;
    if (nargs >= 3) {
        int64_t s = POP_I();
        start = (s < 1) ? 0 : (int)(s - 1);
    }
    if (!haystack || !needle || haystack[0] == 0 || needle[0] == 0) {
        PUSH_I(0);
        DISPATCH();
    }
    int hlen = haystack[0], nlen = needle[0];
    int found = 0;
    for (int i = start; i <= hlen - nlen; i++) {
        if (memcmp(&haystack[1 + i], &needle[1], nlen) == 0) {
            found = i + 1;  /* 1-based */
            break;
        }
    }
    PUSH_I((int64_t)found);
    DISPATCH();
}

op_str_hex: {
    /* HEX$(n%) -> str$ */
    int64_t n = POP_I();
    uint8_t *temp = STR_TEMP();
    char buf[32];
    if (n < 0) {
        /* For negative numbers, print full 64-bit hex */
        snprintf(buf, sizeof(buf), "%llX", (unsigned long long)(uint64_t)n);
    } else {
        snprintf(buf, sizeof(buf), "%llX", (unsigned long long)n);
    }
    int blen = (int)strlen(buf);
    temp[0] = (uint8_t)blen;
    memcpy(temp + 1, buf, blen);
    PUSH_S(temp);
    DISPATCH();
}

op_str_oct: {
    /* OCT$(n%) -> str$ */
    int64_t n = POP_I();
    uint8_t *temp = STR_TEMP();
    char buf[32];
    snprintf(buf, sizeof(buf), "%llo", (unsigned long long)(uint64_t)n);
    int blen = (int)strlen(buf);
    temp[0] = (uint8_t)blen;
    memcpy(temp + 1, buf, blen);
    PUSH_S(temp);
    DISPATCH();
}

op_str_bin: {
    /* BIN$(n%) -> str$ */
    int64_t n = POP_I();
    uint8_t *temp = STR_TEMP();
    uint64_t v = (uint64_t)n;
    if (v == 0) {
        temp[0] = 1; temp[1] = '0';
    } else {
        char buf[65];
        int pos = 0;
        /* Find highest bit */
        int bits = 64;
        while (bits > 0 && !((v >> (bits - 1)) & 1)) bits--;
        for (int i = bits - 1; i >= 0; i--)
            buf[pos++] = ((v >> i) & 1) ? '1' : '0';
        temp[0] = (uint8_t)pos;
        memcpy(temp + 1, buf, pos);
    }
    PUSH_S(temp);
    DISPATCH();
}

    /* ==================================================================
     * Native Math Functions
     * ================================================================== */

op_math_sin: {
    MMFLOAT v = POP_F();
    PUSH_F(sin(v));
    DISPATCH();
}

op_math_cos: {
    MMFLOAT v = POP_F();
    PUSH_F(cos(v));
    DISPATCH();
}

op_math_tan: {
    MMFLOAT v = POP_F();
    PUSH_F(tan(v));
    DISPATCH();
}

op_math_atn: {
    MMFLOAT v = POP_F();
    PUSH_F(atan(v));
    DISPATCH();
}

op_math_sqr: {
    MMFLOAT v = POP_F();
    if (v < 0.0) bc_vm_error(vm, "SQR of negative number");
    PUSH_F(sqrt(v));
    DISPATCH();
}

op_math_log: {
    MMFLOAT v = POP_F();
    if (v <= 0.0) bc_vm_error(vm, "LOG of non-positive number");
    PUSH_F(log(v));
    DISPATCH();
}

op_math_exp: {
    MMFLOAT v = POP_F();
    PUSH_F(exp(v));
    DISPATCH();
}

op_math_abs: {
    /* Preserves type: if TOS is int, result is int; if float, result is float */
    if (vm->stack_types[vm->sp] == T_INT) {
        int64_t v = vm->stack[vm->sp].i;
        vm->stack[vm->sp].i = (v < 0) ? -v : v;
    } else {
        MMFLOAT v = vm->stack[vm->sp].f;
        vm->stack[vm->sp].f = fabs(v);
    }
    DISPATCH();
}

op_math_sgn: {
    /* SGN returns -1, 0, or 1 as integer */
    if (vm->stack_types[vm->sp] == T_INT) {
        int64_t v = vm->stack[vm->sp].i;
        vm->stack[vm->sp].i = (v > 0) ? 1 : (v < 0) ? -1 : 0;
    } else {
        MMFLOAT v = vm->stack[vm->sp].f;
        vm->stack[vm->sp].i = (v > 0.0) ? 1 : (v < 0.0) ? -1 : 0;
        vm->stack_types[vm->sp] = T_INT;
    }
    DISPATCH();
}

op_math_int: {
    /* INT(x) = floor(x), returns float */
    MMFLOAT v = POP_F();
    PUSH_F(floor(v));
    DISPATCH();
}

op_math_fix: {
    /* FIX(x) = truncate toward zero, returns int */
    MMFLOAT v = POP_F();
    PUSH_I((int64_t)v);
    DISPATCH();
}

op_math_cint: {
    /* CINT(x) = round to nearest integer */
    MMFLOAT v = POP_F();
    PUSH_I((int64_t)(v + (v >= 0.0 ? 0.5 : -0.5)));
    DISPATCH();
}

op_math_rad: {
    /* RAD(degrees) -> radians */
    MMFLOAT v = POP_F();
    PUSH_F(v * M_PI / 180.0);
    DISPATCH();
}

op_math_deg: {
    /* DEG(radians) -> degrees */
    MMFLOAT v = POP_F();
    PUSH_F(v * 180.0 / M_PI);
    DISPATCH();
}

op_math_pi: {
    /* PI — no argument, push pi */
    PUSH_F(M_PI);
    DISPATCH();
}

op_math_max: {
    /* MAX(a, b) -> float */
    MMFLOAT b = POP_F();
    MMFLOAT a = POP_F();
    PUSH_F(a > b ? a : b);
    DISPATCH();
}

op_math_min: {
    /* MIN(a, b) -> float */
    MMFLOAT b = POP_F();
    MMFLOAT a = POP_F();
    PUSH_F(a < b ? a : b);
    DISPATCH();
}

    /* ==================================================================
     * DATA / READ / RESTORE
     * ================================================================== */

op_read_i: {
    /* Read next data item, convert to integer, push on stack */
    BCCompiler *cs = vm->compiler;
    if (vm->data_ptr >= cs->data_count)
        bc_vm_error(vm, "No DATA to read");
    BCDataItem *item = &cs->data_pool[vm->data_ptr++];
    int64_t val;
    if (item->type == T_INT)
        val = item->value.i;
    else if (item->type == T_NBR)
        val = (int64_t)item->value.f;
    else
        bc_vm_error(vm, "Type mismatch in READ (expected number, got string)");
    PUSH_I(val);
    DISPATCH();
}

op_read_f: {
    /* Read next data item, convert to float, push on stack */
    BCCompiler *cs = vm->compiler;
    if (vm->data_ptr >= cs->data_count)
        bc_vm_error(vm, "No DATA to read");
    BCDataItem *item = &cs->data_pool[vm->data_ptr++];
    MMFLOAT val;
    if (item->type == T_NBR)
        val = item->value.f;
    else if (item->type == T_INT)
        val = (MMFLOAT)item->value.i;
    else
        bc_vm_error(vm, "Type mismatch in READ (expected number, got string)");
    PUSH_F(val);
    DISPATCH();
}

op_read_s: {
    /* Read next data item as string, push on stack */
    BCCompiler *cs = vm->compiler;
    if (vm->data_ptr >= cs->data_count)
        bc_vm_error(vm, "No DATA to read");
    BCDataItem *item = &cs->data_pool[vm->data_ptr++];
    if (item->type == T_STR) {
        /* String stored as constant pool index */
        uint16_t cidx = (uint16_t)item->value.i;
        BCConstant *c = &cs->constants[cidx];
        uint8_t *buf = vm_get_str_temp(vm);
        buf[0] = (uint8_t)c->len;
        memcpy(buf + 1, c->data, c->len);
        PUSH_S(buf);
    } else {
        /* Convert number to string */
        uint8_t *buf = vm_get_str_temp(vm);
        char tmp[64];
        if (item->type == T_INT)
            snprintf(tmp, sizeof(tmp), "%lld", (long long)item->value.i);
        else
            snprintf(tmp, sizeof(tmp), "%g", item->value.f);
        int len = strlen(tmp);
        buf[0] = (uint8_t)len;
        memcpy(buf + 1, tmp, len);
        PUSH_S(buf);
    }
    DISPATCH();
}

op_restore: {
    vm->data_ptr = 0;
    DISPATCH();
}

    /* ==================================================================
     * Additional string functions
     * ================================================================== */

op_str_space: {
    /* SPACE$(n%) -> str$ of n spaces */
    int64_t n = POP_I();
    if (n < 0 || n > MAXSTRLEN) bc_vm_error(vm, "SPACE$ count out of range");
    uint8_t *buf = STR_TEMP();
    memset(buf + 1, ' ', (int)n);
    buf[0] = (uint8_t)n;
    PUSH_S(buf);
    DISPATCH();
}

op_str_string: {
    /* STRING$(n%, char%) -> str$ of n copies of char */
    int64_t ch = POP_I();
    int64_t n = POP_I();
    if (n < 0 || n > MAXSTRLEN) bc_vm_error(vm, "STRING$ count out of range");
    if (ch < 0 || ch > 255) bc_vm_error(vm, "STRING$ char out of range");
    uint8_t *buf = STR_TEMP();
    memset(buf + 1, (int)ch, (int)n);
    buf[0] = (uint8_t)n;
    PUSH_S(buf);
    DISPATCH();
}

op_str_inkey: {
    /* INKEY$ -> str$ (0 or 1 char) */
    uint8_t *buf = STR_TEMP();
    int i = MMInkey();
    if (i != -1) {
        buf[0] = 1;
        buf[1] = (uint8_t)i;
    } else {
        buf[0] = 0;
    }
    PUSH_S(buf);
    DISPATCH();
}

    /* ==================================================================
     * Additional numeric functions
     * ================================================================== */

op_rnd: {
    /* RND -> float 0.0 <= x < 1.0 */
    MMFLOAT f = (MMFLOAT)rand() / ((MMFLOAT)RAND_MAX + (MMFLOAT)RAND_MAX / 1000000);
    PUSH_F(f);
    DISPATCH();
}

    /* ==================================================================
     * Additional statements
     * ================================================================== */

op_randomize: {
    /* RANDOMIZE seed — pop int seed (0 = use time) */
    int64_t seed = POP_I();
    if (seed == 0) {
#ifdef MMBASIC_HOST
        seed = 42;  /* deterministic for testing */
#else
        seed = time_us_32();
#endif
    }
    if (seed < 0) bc_vm_error(vm, "Number out of bounds");
    srand((unsigned int)seed);
    DISPATCH();
}

op_error_s: {
    /* ERROR "message" — pop string, raise error */
    uint8_t *s = POP_S();
    char buf[STRINGSIZE];
    int len = s ? s[0] : 0;
    if (len > 0) memcpy(buf, s + 1, len);
    buf[len] = 0;
    error(buf);
    DISPATCH();  /* unreachable, error() longjmps */
}

op_error_empty: {
    /* ERROR — raise empty error */
    error("");
    DISPATCH();  /* unreachable */
}

op_clear: {
    /* CLEAR — wipe all variables */
    ClearVars(0, true);
    DISPATCH();
}

    /* ==================================================================
     * Housekeeping
     * ================================================================== */

op_line: {
    uint16_t lineno = READ_U16();
    vm->current_line = lineno;
    DISPATCH();
}

op_checkint:
    CheckAbort();
    check_interrupt();
    DISPATCH();

op_end:
    /* Clean up dynamically allocated arrays */
    for (int i = 0; i < BC_MAX_SLOTS; i++) {
        if (vm->arrays[i].data) {
            BC_FREE(vm->arrays[i].data);
            vm->arrays[i].data = NULL;
        }
    }
    return;

op_halt:
    vm_output(vm, "STOP\r\n");
    /* Clean up dynamically allocated arrays */
    for (int i = 0; i < BC_MAX_SLOTS; i++) {
        if (vm->arrays[i].data) {
            BC_FREE(vm->arrays[i].data);
            vm->arrays[i].data = NULL;
        }
    }
    return;

op_invalid:
    bc_vm_error(vm, "Invalid opcode 0x%02X at offset %u",
                *(vm->pc - 1),
                (unsigned)(vm->pc - 1 - vm->bytecode));

    /* Undefine local macros */
#undef DISPATCH
#undef READ_U16
#undef READ_I16
#undef READ_U32
#undef READ_I64
#undef READ_F64
#undef PUSH_I
#undef PUSH_F
#undef PUSH_S
#undef POP_I
#undef POP_F
#undef POP_S
#undef TOS_I
#undef TOS_F
}
