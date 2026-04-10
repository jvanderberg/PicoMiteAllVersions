/*
 * bytecode.h - Bytecode VM definitions for FRUN command
 *
 * Defines the instruction set, compiler state, and VM state for the
 * MMBasic bytecode compiler and virtual machine.
 *
 * All variable types must be explicit (%, !, $) — no implicit typing.
 */

#ifndef __BYTECODE_H
#define __BYTECODE_H

#include "MMBasic.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opcode definitions — variable-length encoding: [opcode:8][operands...]
 * Multi-byte operands are little-endian (ARM native).
 */
typedef enum {
    /* Stack / Value Operations */
    OP_NOP          = 0x00,
    OP_PUSH_INT     = 0x01,  /* i64 (8 bytes) */
    OP_PUSH_FLT     = 0x02,  /* f64 (8 bytes) */
    OP_PUSH_STR     = 0x03,  /* idx:16 — string from constant pool */
    OP_PUSH_ZERO    = 0x04,  /* — push integer 0 */
    OP_PUSH_ONE     = 0x05,  /* — push integer 1 */
    OP_LOAD_I       = 0x06,  /* slot:16 — load integer global */
    OP_LOAD_F       = 0x07,  /* slot:16 — load float global */
    OP_LOAD_S       = 0x08,  /* slot:16 — load string global */
    OP_STORE_I      = 0x09,  /* slot:16 — pop → integer global */
    OP_STORE_F      = 0x0A,  /* slot:16 — pop → float global */
    OP_STORE_S      = 0x0B,  /* slot:16 — pop → string global */
    OP_LOAD_ARR_I   = 0x0C,  /* slot:16, ndim:8 — load int array elem */
    OP_LOAD_ARR_F   = 0x0D,  /* slot:16, ndim:8 — load float array elem */
    OP_LOAD_ARR_S   = 0x0E,  /* slot:16, ndim:8 — load string array elem */
    OP_STORE_ARR_I  = 0x0F,  /* slot:16, ndim:8 — store int array elem */
    OP_STORE_ARR_F  = 0x10,  /* slot:16, ndim:8 — store float array elem */
    OP_STORE_ARR_S  = 0x11,  /* slot:16, ndim:8 — store string array elem */
    OP_POP          = 0x12,  /* — discard TOS */
    OP_DUP          = 0x13,  /* — duplicate TOS */
    OP_CVT_I2F      = 0x14,  /* — convert TOS int → float */
    OP_CVT_F2I      = 0x15,  /* — convert TOS float → int */

    /* Integer Arithmetic (pop 2, push 1) */
    OP_ADD_I        = 0x20,
    OP_SUB_I        = 0x21,
    OP_MUL_I        = 0x22,
    OP_IDIV_I       = 0x23,  /* integer divide (\) */
    OP_MOD_I        = 0x24,

    /* Float Arithmetic (pop 2, push 1) */
    OP_ADD_F        = 0x28,
    OP_SUB_F        = 0x29,
    OP_MUL_F        = 0x2A,
    OP_DIV_F        = 0x2B,
    OP_POW_F        = 0x2C,
    OP_MOD_F        = 0x2D,

    /* String Operations */
    OP_ADD_S        = 0x30,  /* concatenate (pop 2, push 1) */

    /* Unary (pop 1, push 1) */
    OP_NEG_I        = 0x38,
    OP_NEG_F        = 0x39,
    OP_NOT          = 0x3A,  /* logical NOT (integer) */
    OP_INV          = 0x3B,  /* bitwise NOT ~ (integer) */

    /* Bitwise / Logical (pop 2, push 1) */
    OP_AND          = 0x40,
    OP_OR           = 0x41,
    OP_XOR          = 0x42,
    OP_SHL          = 0x43,  /* << */
    OP_SHR          = 0x44,  /* >> */

    /* Integer Comparison (pop 2, push int 0 or 1) */
    OP_EQ_I         = 0x48,
    OP_NE_I         = 0x49,
    OP_LT_I         = 0x4A,
    OP_GT_I         = 0x4B,
    OP_LE_I         = 0x4C,
    OP_GE_I         = 0x4D,

    /* Float Comparison (pop 2, push int 0 or 1) */
    OP_EQ_F         = 0x50,
    OP_NE_F         = 0x51,
    OP_LT_F         = 0x52,
    OP_GT_F         = 0x53,
    OP_LE_F         = 0x54,
    OP_GE_F         = 0x55,

    /* String Comparison (pop 2, push int 0 or 1) */
    OP_EQ_S         = 0x58,
    OP_NE_S         = 0x59,
    OP_LT_S         = 0x5A,
    OP_GT_S         = 0x5B,
    OP_LE_S         = 0x5C,
    OP_GE_S         = 0x5D,

    /* Control Flow */
    OP_JMP          = 0x60,  /* offset:16 — relative jump */
    OP_JMP_ABS      = 0x61,  /* addr:32 — absolute jump */
    OP_JZ           = 0x62,  /* offset:16 — jump if TOS == 0 */
    OP_JNZ          = 0x63,  /* offset:16 — jump if TOS != 0 */
    OP_GOSUB        = 0x64,  /* addr:32 — push return, jump */
    OP_RETURN       = 0x65,

    /* FOR loop compound opcodes */
    OP_FOR_INIT_I   = 0x66,  /* var:16, lim:16, step:16, exit_off:16 */
    OP_FOR_NEXT_I   = 0x67,  /* var:16, lim:16, step:16, loop_off:16 */
    OP_FOR_INIT_F   = 0x68,  /* var:16, lim:16, step:16, exit_off:16 */
    OP_FOR_NEXT_F   = 0x69,  /* var:16, lim:16, step:16, loop_off:16 */

    /* SUB / FUNCTION */
    OP_CALL_SUB     = 0x70,  /* idx:16, nargs:8 */
    OP_CALL_FUN     = 0x71,  /* idx:16, nargs:8 */
    OP_RET_SUB      = 0x72,
    OP_RET_FUN      = 0x73,  /* result on stack */
    OP_ENTER_FRAME  = 0x74,  /* nlocals:16 */
    OP_LEAVE_FRAME  = 0x75,
    OP_LOAD_LOCAL_I = 0x76,  /* offset:16 */
    OP_LOAD_LOCAL_F = 0x77,  /* offset:16 */
    OP_LOAD_LOCAL_S = 0x78,  /* offset:16 */
    OP_STORE_LOCAL_I= 0x79,  /* offset:16 */
    OP_STORE_LOCAL_F= 0x7A,  /* offset:16 */
    OP_STORE_LOCAL_S= 0x7B,  /* offset:16 */
    OP_LOAD_LOCAL_ARR_I  = 0x7C, /* offset:16, ndim:8 */
    OP_LOAD_LOCAL_ARR_F  = 0x7D, /* offset:16, ndim:8 */
    OP_LOAD_LOCAL_ARR_S  = 0x7E, /* offset:16, ndim:8 */
    OP_STORE_LOCAL_ARR_I = 0x7F, /* offset:16, ndim:8 (unused in this range but reserved) */

    /* Built-in Bridge */
    OP_BUILTIN_CMD  = 0x80,  /* idx:16 */
    OP_BUILTIN_FUN_I= 0x81,  /* idx:16, nargs:8 */
    OP_BUILTIN_FUN_F= 0x82,  /* idx:16, nargs:8 */
    OP_BUILTIN_FUN_S= 0x83,  /* idx:16, nargs:8 */

    /* PRINT */
    OP_PRINT_INT    = 0x88,  /* flags:8 (bit0=no newline, bit1=tab after) */
    OP_PRINT_FLT    = 0x89,  /* flags:8 */
    OP_PRINT_STR    = 0x8A,  /* flags:8 */
    OP_PRINT_NEWLINE= 0x8B,  /* — emit CR/LF */
    OP_PRINT_TAB    = 0x8C,  /* — emit tab */

    /* DIM arrays */
    OP_DIM_ARR_I    = 0x90,  /* slot:16, ndim:8 — sizes on stack */
    OP_DIM_ARR_F    = 0x91,  /* slot:16, ndim:8 */
    OP_DIM_ARR_S    = 0x92,  /* slot:16, ndim:8 */

    /* Native string functions (compiled arguments, no bridge) */
    OP_STR_LEN      = 0xA0,  /* pop str, push int len */
    OP_STR_LEFT     = 0xA1,  /* pop int n, pop str, push str LEFT$(s,n) */
    OP_STR_RIGHT    = 0xA2,  /* pop int n, pop str, push str RIGHT$(s,n) */
    OP_STR_MID2     = 0xA3,  /* pop int start, pop str, push str MID$(s,start) */
    OP_STR_MID3     = 0xA4,  /* pop int len, pop int start, pop str, push str MID$(s,start,len) */
    OP_STR_UCASE    = 0xA5,  /* pop str, push str UCASE$(s) */
    OP_STR_LCASE    = 0xA6,  /* pop str, push str LCASE$(s) */
    OP_STR_VAL      = 0xA7,  /* pop str, push float VAL(s) */
    OP_STR_STR      = 0xA8,  /* pop float, push str STR$(n) */
    OP_STR_CHR      = 0xA9,  /* pop int, push str CHR$(n) */
    OP_STR_ASC      = 0xAA,  /* pop str, push int ASC(s) */
    OP_STR_INSTR    = 0xAB,  /* nargs:8 — INSTR([start%,] haystack$, needle$) */
    OP_STR_HEX      = 0xAC,  /* pop int, push str HEX$(n) */
    OP_STR_OCT      = 0xAD,  /* pop int, push str OCT$(n) */
    OP_STR_BIN      = 0xAE,  /* pop int, push str BIN$(n) */

    /* Native math functions (compiled arguments, no bridge) */
    OP_MATH_SIN     = 0xB0,  /* pop float, push float SIN(x) */
    OP_MATH_COS     = 0xB1,  /* pop float, push float COS(x) */
    OP_MATH_TAN     = 0xB2,  /* pop float, push float TAN(x) */
    OP_MATH_ATN     = 0xB3,  /* pop float, push float ATN(x) */
    OP_MATH_SQR     = 0xB4,  /* pop float, push float SQR(x) */
    OP_MATH_LOG     = 0xB5,  /* pop float, push float LOG(x) */
    OP_MATH_EXP     = 0xB6,  /* pop float, push float EXP(x) */
    OP_MATH_ABS     = 0xB7,  /* pop num, push num ABS(x) — preserves type */
    OP_MATH_SGN     = 0xB8,  /* pop num, push int SGN(x) */
    OP_MATH_INT     = 0xB9,  /* pop float, push float INT(x) — floor */
    OP_MATH_FIX     = 0xBA,  /* pop float, push int FIX(x) — truncate */
    OP_MATH_CINT    = 0xBB,  /* pop float, push int CINT(x) — round */
    OP_MATH_RAD     = 0xBC,  /* pop float, push float RAD(x) */
    OP_MATH_DEG     = 0xBD,  /* pop float, push float DEG(x) */
    OP_MATH_PI      = 0xBE,  /* push float PI */
    OP_MATH_MAX     = 0xBF,  /* pop float b, pop float a, push float MAX */
    OP_MATH_MIN     = 0xC0,  /* pop float b, pop float a, push float MIN */

    /* DATA / READ / RESTORE */
    OP_READ_I       = 0xC1,  /* — push next data item as int */
    OP_READ_F       = 0xC2,  /* — push next data item as float */
    OP_READ_S       = 0xC3,  /* — push next data item as string */
    OP_RESTORE      = 0xC4,  /* — reset data pointer to 0 */

    /* Additional string functions */
    OP_STR_SPACE    = 0xC5,  /* pop int n, push str SPACE$(n) */
    OP_STR_STRING   = 0xC6,  /* pop int char, pop int n, push str STRING$(n,c) */
    OP_STR_INKEY    = 0xC7,  /* — push str INKEY$ */

    /* Additional numeric functions */
    OP_RND          = 0xC8,  /* — push float RND */

    /* Additional statements */
    OP_INC_I        = 0xC9,  /* slot:16 — INC integer var by TOS or 1 */
    OP_INC_F        = 0xCA,  /* slot:16 — INC float var by TOS or 1 */
    OP_RANDOMIZE    = 0xCB,  /* — pop int seed, RANDOMIZE */
    OP_ERROR_S      = 0xCC,  /* — pop string, raise error */
    OP_ERROR_EMPTY  = 0xCD,  /* — raise empty error */
    OP_CLEAR        = 0xCE,  /* — CLEAR all variables */

    /* Housekeeping */
    OP_LINE         = 0xF0,  /* lineno:16 — for errors/trace */
    OP_CHECKINT     = 0xF1,  /* — check CTRL-C / interrupts */
    OP_END          = 0xFE,
    OP_HALT         = 0xFF,  /* STOP statement */
} BCOpcode;

/* Print flags */
#define PRINT_NO_NEWLINE  0x01
#define PRINT_TAB_AFTER   0x02
#define PRINT_SEMICOLON   0x04

/*
 * Compiler limits — platform-conditional
 *
 * Host build uses generous limits for comprehensive testing.
 * RP2350 device builds get a larger compiler budget to match their larger heap.
 * Other device builds keep reduced limits to fit comfortably in the RP2040 heap.
 * Compiler arrays are dynamically allocated via bc_compiler_alloc().
 */
#ifdef MMBASIC_HOST
  #define BC_MAX_CODE       (64 * 1024)
  #define BC_MAX_CONSTANTS  512
  #define BC_MAX_SLOTS      512
  #define BC_MAX_SUBFUNS    256
  #define BC_MAX_FIXUPS     2048
  #define BC_MAX_LINEMAP    4096
  #define BC_MAX_LOCALS     64
  #define BC_MAX_LOCAL_META 4096
  #define BC_MAX_NEST       64
  #define BC_MAX_DATA_ITEMS 1024
#elif defined(rp2350)
  /*
   * The RP2350 firmware has a substantially larger heap than RP2040 builds,
   * but host-sized compiler tables would still be wasteful on-device.
   * These limits keep the compiler/VM metadata comfortably below the RP2350
   * heap budget while removing several host/device mismatches.
   */
  #define BC_MAX_CODE       (32 * 1024)
  #define BC_MAX_CONSTANTS  128
  #define BC_MAX_SLOTS      256
  #define BC_MAX_SUBFUNS    128
  #define BC_MAX_FIXUPS     1024
  #define BC_MAX_LINEMAP    2048
  #define BC_MAX_LOCALS     64
  #define BC_MAX_LOCAL_META 1024
  #define BC_MAX_NEST       32
  #define BC_MAX_DATA_ITEMS 1024
#else
  #define BC_MAX_CODE       (16 * 1024)
  #define BC_MAX_CONSTANTS  32
  #define BC_MAX_SLOTS      128
  #define BC_MAX_SUBFUNS    32
  #define BC_MAX_FIXUPS     256
  #define BC_MAX_LINEMAP    512
  #define BC_MAX_LOCALS     64
  #define BC_MAX_LOCAL_META 256
  #define BC_MAX_NEST       16
  #define BC_MAX_DATA_ITEMS 512
#endif

/*
 * Variable slot — compile-time record
 */
typedef struct {
    char     name[MAXVARLEN + 1];
    uint8_t  type;              /* T_INT, T_NBR, T_STR */
    uint8_t  is_array;
    uint8_t  ndims;
    int      dims[MAXDIM];      /* array dimension sizes, 0 if unknown at compile time */
} BCSlot;

/*
 * SUB/FUNCTION record
 */
typedef struct {
    char     name[MAXVARLEN + 1];
    uint32_t entry_addr;        /* bytecode offset of ENTER_FRAME */
    uint8_t  nparams;
    uint8_t  param_types[BC_MAX_LOCALS]; /* T_INT, T_NBR, T_STR for each param */
    uint8_t  param_is_array[BC_MAX_LOCALS]; /* 1 if param is array (passed by ref) */
    uint8_t  return_type;       /* 0 for SUB, T_INT/T_NBR/T_STR for FUNCTION */
    uint16_t nlocals;           /* total local slots (params + LOCAL vars) */
} BCSubFun;

/*
 * String constant pool entry
 */
typedef struct {
    uint8_t  data[STRINGSIZE];
    uint16_t len;
} BCConstant;

/*
 * Forward reference fixup
 */
typedef struct {
    uint32_t patch_addr;        /* offset in code[] to patch */
    int      target_line;       /* line number to resolve to (for GOTO/GOSUB) */
    int      target_label;      /* -1 if using line number */
    uint8_t  size;              /* 2 or 4 byte patch */
    uint8_t  is_relative;       /* 1 = relative offset, 0 = absolute addr */
} BCFixup;

/*
 * Line number → bytecode offset mapping
 */
typedef struct {
    uint16_t lineno;
    uint32_t offset;
} BCLineMap;

/*
 * Control flow nesting stack (used during compilation)
 */
typedef enum {
    NEST_IF,
    NEST_FOR,
    NEST_DO,
    NEST_WHILE,
    NEST_SELECT,
    NEST_SUB,
    NEST_FUNCTION,
} BCNestType;

typedef struct {
    BCNestType type;
    uint32_t   addr1;           /* for IF: addr of JZ patch; FOR: loop top; DO: loop top */
    uint32_t   addr2;           /* for IF: addr of JMP-to-endif patch */
    uint32_t   addr3;           /* extra (e.g., SELECT temp slot) */
    uint16_t   var_slot;        /* FOR: loop variable slot */
    uint16_t   lim_slot;        /* FOR: limit hidden slot */
    uint16_t   step_slot;       /* FOR: step hidden slot */
    uint8_t    var_type;        /* FOR: T_INT or T_NBR */
    uint8_t    has_else;        /* IF: whether ELSE was seen */

    /* For SELECT CASE */
    uint16_t   select_slot;     /* hidden variable slot for select expr */
    uint8_t    select_type;     /* type of select expression */
    uint32_t   case_end_fixups[32]; /* fixup addrs for JMP to END SELECT */
    int        case_end_count;

    /* For EXIT FOR/DO — patch locations to fill in when we reach NEXT/LOOP */
    uint32_t   exit_fixups[16];
    int        exit_fixup_count;
} BCNestEntry;

/*
 * DATA pool item — one per value in DATA statements
 * Uses raw union instead of BCValue to avoid forward-declaration issues.
 */
typedef struct {
    union {
        MMFLOAT f;
        int64_t i;
    } value;
    uint8_t  type;    /* T_INT, T_NBR, or T_STR (for T_STR, .i = const pool index) */
} BCDataItem;

/*
 * Persisted local variable metadata for bridged command/function evaluation.
 */
typedef struct {
    char    name[MAXVARLEN + 1];
    uint8_t type;
    uint8_t is_array;
} BCLocalMeta;

/*
 * Local variable record (used during compilation)
 */
typedef struct {
    char    name[MAXVARLEN + 1];
    uint8_t type;
    uint8_t is_array;
} BCLocalVar;

/*
 * Compiler state
 *
 * All large arrays are dynamically allocated via bc_compiler_alloc().
 * On host: calloc/free.  On device: GetMemory/FreeMemory from MMBasic heap.
 */
typedef struct {
    /* Output bytecode (allocated: BC_MAX_CODE bytes) */
    uint8_t    *code;
    uint32_t   code_len;

    /* Constant pool (allocated: BC_MAX_CONSTANTS entries) */
    BCConstant *constants;
    uint16_t   const_count;

    /* Global variable slots (allocated: BC_MAX_SLOTS entries) */
    BCSlot     *slots;
    uint16_t   slot_count;
    uint16_t   next_hidden_slot;   /* for compiler-generated temporaries */

    /* SUB/FUNCTION table (allocated: BC_MAX_SUBFUNS entries) */
    BCSubFun   *subfuns;
    uint16_t   subfun_count;
    uint16_t   *subfun_locals_base;

    /* Forward reference fixups (allocated: BC_MAX_FIXUPS entries) */
    BCFixup    *fixups;
    uint16_t   fixup_count;

    /* Line map (allocated: BC_MAX_LINEMAP entries) */
    BCLineMap  *linemap;
    uint16_t   linemap_count;

    /* Control flow nesting stack (allocated: BC_MAX_NEST entries) */
    BCNestEntry *nest_stack;
    int         nest_depth;

    /* Current context */
    int        current_subfun;     /* -1 if not in SUB/FUNCTION */
    uint16_t   current_line;

    /* Local variable tracking (allocated: BC_MAX_LOCALS entries) */
    BCLocalVar *locals;
    uint16_t   local_count;

    /* Persisted local metadata for all compiled SUB/FUNCTIONs */
    BCLocalMeta *local_meta;
    uint16_t    local_meta_count;

    /* DATA pool (allocated: BC_MAX_DATA_ITEMS entries) */
    BCDataItem *data_pool;
    uint16_t   data_count;

    /* Error state */
    int        error_line;
    char       error_msg[128];
    int        has_error;
} BCCompiler;


/*
 * VM runtime value
 */
typedef union {
    MMFLOAT     f;
    int64_t     i;
    uint8_t    *s;       /* MMBasic format string (length prefix) */
} BCValue;

/*
 * VM call stack frame
 */
typedef struct {
    uint8_t    *return_pc;
    int         frame_base;     /* index into locals[] */
    int         locals_top;     /* caller's live local extent */
    int         saved_sp;
    uint16_t    nlocals;        /* number of locals in this frame */
    uint16_t    subfun_idx;     /* active SUB/FUNCTION metadata */
} BCCallFrame;

/*
 * VM FOR stack entry
 */
typedef struct {
    uint16_t    var_slot;
    uint16_t    lim_slot;
    uint16_t    step_slot;
    uint8_t    *loop_top;      /* PC of loop body start */
    uint8_t     is_local;      /* variable is local (not global) */
    uint8_t     var_type;      /* T_INT or T_NBR */
} BCForEntry;

/*
 * VM array storage
 */
typedef struct {
    BCValue    *data;           /* allocated array of BCValues */
    int         dims[MAXDIM];   /* dimension sizes */
    uint8_t     ndims;
    uint8_t     elem_type;      /* T_INT, T_NBR, T_STR */
    uint32_t    total_elements;
} BCArray;

/*
 * VM state
 */
#define VM_STACK_SIZE    256
#define VM_MAX_CALL      64
#define VM_MAX_FOR       32
#define VM_MAX_GOSUB     64

#ifdef MMBASIC_HOST
  #define VM_MAX_LOCALS   1024
#else
  #define VM_MAX_LOCALS   256    /* supports ~4 recursion levels * 64 locals */
#endif

/*
 * VM state
 *
 * Large arrays (globals, arrays, locals, local_arrays) are dynamically
 * allocated via bc_vm_alloc().  Small fixed-size arrays stay inline.
 */
typedef struct {
    uint8_t    *pc;             /* program counter into bytecode */

    /* Operand stack for expression evaluation (inline — small, fixed) */
    BCValue     stack[VM_STACK_SIZE];
    uint8_t     stack_types[VM_STACK_SIZE];
    int         sp;             /* stack pointer (-1 = empty) */

    /* Global variable storage (allocated: BC_MAX_SLOTS entries) */
    BCValue    *globals;
    uint8_t    *global_types;   /* tracks what's stored */

    /* Array storage for globals (allocated: BC_MAX_SLOTS entries) */
    BCArray    *arrays;         /* parallel to globals[], used if is_array */

    /* Call stack (inline — small, fixed) */
    BCCallFrame call_stack[VM_MAX_CALL];
    int         csp;            /* call stack pointer */

    /* GOSUB stack (inline — small, fixed) */
    struct {
        uint8_t *return_pc;
    } gosub_stack[VM_MAX_GOSUB];
    int         gsp;            /* gosub stack pointer */

    /* Local variable frames (allocated: VM_MAX_LOCALS entries) */
    BCValue    *locals;
    uint8_t    *local_types;
    int         frame_base;     /* current frame base in locals[] */
    int         locals_top;     /* next free slot in locals[] */

    /* Local array storage (allocated: VM_MAX_LOCALS entries) */
    BCArray    *local_arrays;   /* parallel to locals[] */

    /* FOR loop stack (inline — small, fixed) */
    BCForEntry  for_stack[VM_MAX_FOR];
    int         fsp;

    /* Bytecode and metadata (pointers to compiler output) */
    uint8_t    *bytecode;
    uint32_t    bytecode_len;
    BCCompiler *compiler;       /* for constant pool, linemap, etc. */

    /* DATA read pointer */
    uint16_t    data_ptr;       /* current position in compiler->data_pool[] */

    /* Error reporting */
    uint16_t    current_line;

    /* String temp storage (inline — small, fixed) */
    uint8_t     str_temp[4][STRINGSIZE];
    int         str_temp_idx;

    /* Output capture (for FTEST comparison) */
    char       *capture_buf;    /* if non-NULL, PRINT writes here instead of console */
    int         capture_len;
    int         capture_cap;
} BCVMState;


/*
 * Public API
 */

/* Compiler */
int  bc_compiler_alloc(BCCompiler *cs);   /* allocate all dynamic arrays */
void bc_compiler_free(BCCompiler *cs);    /* free all dynamic arrays */
void bc_compiler_compact(BCCompiler *cs); /* shrink to actual size after compile */
void bc_compiler_init(BCCompiler *cs);    /* reset state (arrays must be allocated) */
int  bc_compile(BCCompiler *cs, unsigned char *prog_memory, int prog_size);

/* VM */
int  bc_vm_alloc(BCVMState *vm);    /* allocate dynamic arrays */
void bc_vm_free(BCVMState *vm);     /* free dynamic arrays */
void bc_vm_init(BCVMState *vm, BCCompiler *cs);
void bc_vm_execute(BCVMState *vm);
void bc_vm_error(BCVMState *vm, const char *msg, ...);

/* Bridge */
void bc_bridge_call_cmd(BCVMState *vm, uint16_t cmd_idx);
void bc_bridge_call_fun(BCVMState *vm, uint16_t fun_idx, uint8_t nargs, uint8_t ret_type);
void bc_bridge_reset_sync(void);

/* Commands */
void cmd_frun(void);
void cmd_ftest(void);

/* Helpers */
uint16_t bc_find_slot(BCCompiler *cs, const char *name, int name_len);
uint16_t bc_add_slot(BCCompiler *cs, const char *name, int name_len, uint8_t type, int is_array);
int      bc_find_subfun(BCCompiler *cs, const char *name, int name_len);
uint16_t bc_add_constant_string(BCCompiler *cs, const uint8_t *data, int len);
int      bc_add_linemap_entry(BCCompiler *cs, uint16_t lineno, uint32_t offset);
int      bc_commit_locals(BCCompiler *cs, int sf_idx);
uint32_t bc_linemap_lookup(BCCompiler *cs, uint16_t lineno);

/* Output capture API (for FTEST comparison) */
void bc_vm_start_capture(BCVMState *vm, char *buf, int capacity);
void bc_vm_capture_write(BCVMState *vm, const char *text, int len);
void bc_vm_capture_char(BCVMState *vm, char c);
void bc_vm_capture_string(BCVMState *vm, const char *s);

/* Bytecode emission helpers */
void bc_emit_byte(BCCompiler *cs, uint8_t b);
void bc_emit_u16(BCCompiler *cs, uint16_t v);
void bc_emit_i16(BCCompiler *cs, int16_t v);
void bc_emit_u32(BCCompiler *cs, uint32_t v);
void bc_emit_ptr(BCCompiler *cs, const void *ptr);
void bc_emit_i64(BCCompiler *cs, int64_t v);
void bc_emit_f64(BCCompiler *cs, MMFLOAT v);
void bc_patch_u16(BCCompiler *cs, uint32_t addr, uint16_t v);
void bc_patch_i16(BCCompiler *cs, uint32_t addr, int16_t v);
void bc_patch_u32(BCCompiler *cs, uint32_t addr, uint32_t v);

/* Debug / diagnostic tools */
extern int bc_debug_enabled;       /* set to 1 to dump stats+disassembly on FRUN */
void bc_disassemble(BCCompiler *cs);
void bc_dump_stats(BCCompiler *cs);
void bc_dump_vm_state(BCVMState *vm);

/* ------------------------------------------------------------------ */
/* Crash diagnostic breadcrumb — survives soft reset via                */
/* __uninitialized_ram on device                                       */
/* ------------------------------------------------------------------ */
#define BC_CRASH_MAGIC 0xDEADC0DE

typedef struct {
    uint32_t magic;         /* BC_CRASH_MAGIC if valid crash data present */
    uint32_t checkpoint;    /* last checkpoint ID reached */
    uint32_t sp;            /* ARM stack pointer at last checkpoint */
    uint32_t cfsr;          /* ARM CFSR (fault status) */
    uint32_t hfsr;          /* ARM HFSR (hard fault status) */
    uint32_t bfar;          /* ARM BFAR (bus fault address) */
    uint32_t mmfar;         /* ARM MMFAR (mem-manage fault address) */
    char     label[32];     /* checkpoint description string */
} BCCrashInfo;

/* Checkpoint stage IDs for cmd_frun() */
#define BC_CK_FRUN_ENTRY       1
#define BC_CK_FRUN_ALLOC_CS    2
#define BC_CK_FRUN_ALLOC_VM    3
#define BC_CK_FRUN_COMP_ALLOC  4
#define BC_CK_FRUN_VM_ALLOC    5
#define BC_CK_FRUN_COMPILE     6
#define BC_CK_FRUN_VM_INIT     7
#define BC_CK_FRUN_EXECUTE     8
#define BC_CK_FRUN_CLEANUP     9

void bc_crash_checkpoint(int stage, const char *label);
void bc_crash_save_fault(void);    /* called from sigbus() to capture ARM regs */
void bc_crash_dump_if_any(void);   /* called at boot to print crash report */
void bc_crash_clear(void);

#ifdef __cplusplus
}
#endif
#endif /* __BYTECODE_H */
