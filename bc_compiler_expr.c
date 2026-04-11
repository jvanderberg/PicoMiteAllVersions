/*
 * bc_compiler_expr.c -- Expression compiler for the bytecode VM
 *
 * Uses the shunting-yard algorithm to compile MMBasic expressions from
 * tokenized source into bytecode.  Called from bc_compile_statement().
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include "bytecode.h"
#include "bc_compiler_internal.h"
#include "Draw.h"
#include "AllCommands.h"

/* ------------------------------------------------------------------ */
/*  Operator stack entry (shunting-yard)                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  token;       /* the single-byte token                    */
    int      precedence;  /* from token table                         */
    uint8_t  is_unary;    /* 1 if unary operator                      */
    void   (*fptr)(void); /* function pointer for identification      */
} OpEntry;

/*
 * Type stack entry.  Each pushed value records its type and the
 * bytecode position right after its code was emitted.  This lets us
 * back-patch an OP_CVT_I2F into the stream when we discover at
 * operator-pop time that the LEFT operand (below TOS) needs promotion.
 * We memmove the right operand's code forward by one byte and insert
 * the CVT at the saved position.
 */
typedef struct {
    uint8_t  type;
    uint32_t code_end;  /* cs->code_len right after this value's code */
} TypeEntry;

#define OP_STACK_MAX   64
#define TYPE_STACK_MAX 64

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */

static uint8_t emit_binary_op(BCCompiler *cs, void (*fptr)(void),
                              uint8_t ltype, uint8_t rtype);
static uint8_t emit_unary_op(BCCompiler *cs, void (*fptr)(void),
                             uint8_t operand_type);
static int     is_expr_end(unsigned char *p, int paren_depth);
static void    parse_number(BCCompiler *cs, unsigned char **pp,
                            uint8_t *type_out);
static void    parse_string(BCCompiler *cs, unsigned char **pp);
static void    skip_balanced_parens(unsigned char **pp);
static int     get_precedence(uint8_t tok);
static int     is_right_assoc(void (*fptr)(void));

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static int is_operator_token(unsigned char c) {
    if (c < C_BASETOKEN) return 0;
    return (tokentype(c) & T_OPER) != 0;
}

static void (*get_token_fptr(unsigned char tok))(void) {
    if (tok >= C_BASETOKEN && tok < TokenTableSize - 1 + C_BASETOKEN)
        return tokentbl[tok - C_BASETOKEN].fptr;
    return NULL;
}

static int is_function_token(unsigned char c) {
    if (c < C_BASETOKEN) return 0;
    return (tokentype(c) & (T_FUN | T_FNA)) != 0;
}

static uint8_t get_function_return_type(unsigned char tok) {
    uint8_t ttype = tokentype(tok);
    if (ttype & T_INT) return T_INT;
    if (ttype & T_STR) return T_STR;
    if (ttype & T_NBR) return T_NBR;
    return T_NBR;
}

/* ------------------------------------------------------------------ */
/*  Expression terminator check                                        */
/* ------------------------------------------------------------------ */

static int is_expr_end(unsigned char *p, int paren_depth) {
    unsigned char c = *p;
    if (c == 0) return 1;
    if (c == ',' && paren_depth <= 0) return 1;
    if (c == ')' && paren_depth <= 0) return 1;
    /* Statement-separating keyword tokens */
    if (c == tokenTHEN || c == tokenELSE || c == tokenTO ||
        c == tokenSTEP || c == tokenWHILE || c == tokenUNTIL ||
        c == tokenGOTO || c == tokenGOSUB || c == tokenAS) {
        return 1;
    }

    /* Two-byte command tokens: both bytes >= C_BASETOKEN and
     * the first byte is NOT a single-byte operator or function token */
    if (c >= C_BASETOKEN && p[1] >= C_BASETOKEN &&
        !(tokentype(c) & (T_OPER | T_FUN | T_FNA)))
        return 1;

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Operator precedence and associativity                              */
/*                                                                     */
/*  MMBasic precedence: lower number = tighter binding.                */
/*  ^ = 0,  * / \ MOD = 1,  + - = 2,  NOT INV = 3,  << >> = 4,      */
/*  <> >= <= < > = 5,  = (equals) = 6,  AND OR XOR = 7.              */
/* ------------------------------------------------------------------ */

static int get_precedence(uint8_t tok) {
    if (tok >= C_BASETOKEN && tok < TokenTableSize - 1 + C_BASETOKEN)
        return (int)tokentbl[tok - C_BASETOKEN].precedence;
    return 99;
}

static int is_right_assoc(void (*fptr)(void)) {
    /* MMBasic treats all operators including ^ as left-associative */
    (void)fptr;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Number parsing -- produces OP_PUSH_INT or OP_PUSH_FLT              */
/* ------------------------------------------------------------------ */

static void parse_number(BCCompiler *cs, unsigned char **pp,
                         uint8_t *type_out) {
    unsigned char *p = *pp;

    /* &H, &O, &B prefixes produce integer */
    if (*p == '&') {
        p++;
        int64_t ival = 0;
        unsigned char bc = (unsigned char)toupper(*p);
        p++;
        switch (bc) {
            case 'H':
                while (isxdigit(*p)) {
                    int d = toupper(*p) >= 'A' ? toupper(*p) - 'A' + 10
                                               : *p - '0';
                    ival = (ival << 4) | d;
                    p++;
                }
                break;
            case 'O':
                while (*p >= '0' && *p <= '7') {
                    ival = (ival << 3) | (*p++ - '0');
                }
                break;
            case 'B':
                while (*p == '0' || *p == '1') {
                    ival = (ival << 1) | (*p++ - '0');
                }
                break;
            default:
                bc_set_error(cs, "Invalid number base prefix &%c", bc);
                *type_out = T_INT;
                *pp = p;
                return;
        }
        if (ival == 0)      bc_emit_byte(cs, OP_PUSH_ZERO);
        else if (ival == 1) bc_emit_byte(cs, OP_PUSH_ONE);
        else { bc_emit_byte(cs, OP_PUSH_INT); bc_emit_i64(cs, ival); }
        *type_out = T_INT;
        *pp = p;
        return;
    }

    /* Decimal number: digits, decimal point, E notation */
    char ts[32];
    char *tsp = ts;
    int is_int = 1;
    int64_t ival = 0;
    int has_scale = 0;
    int64_t scale = 0;

    if (*p == '.') {
        is_int = 0;  has_scale = 1;  scale = 1;
    } else if (*p >= '0' && *p <= '9') {
        ival = *p - '0';
    }
    *tsp++ = (char)*p++;

    while (digit[(uint8_t)*p] && (tsp - ts) < 30) {
        if (*p >= '0' && *p <= '9') {
            ival = ival * 10 + (*p - '0');
            if (has_scale) scale *= 10;
        } else if (*p == '.') {
            is_int = 0;  has_scale = 1;  scale = 1;
        } else {
            /* E, e, +, - inside number body */
            is_int = 0;  has_scale = 0;
        }
        *tsp++ = (char)*p++;
    }
    *tsp = '\0';

    if (is_int) {
        if (ival == 0)      bc_emit_byte(cs, OP_PUSH_ZERO);
        else if (ival == 1) bc_emit_byte(cs, OP_PUSH_ONE);
        else { bc_emit_byte(cs, OP_PUSH_INT); bc_emit_i64(cs, ival); }
        *type_out = T_INT;
    } else {
        MMFLOAT fval;
        if (has_scale && scale > 0 && (tsp - ts) < 18)
            fval = (MMFLOAT)ival / (MMFLOAT)scale;
        else
            fval = (MMFLOAT)strtod(ts, NULL);
        bc_emit_byte(cs, OP_PUSH_FLT);
        bc_emit_f64(cs, fval);
        *type_out = T_NBR;
    }
    *pp = p;
}

/* ------------------------------------------------------------------ */
/*  String literal parsing                                             */
/* ------------------------------------------------------------------ */

static void parse_string(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp;
    p++;  /* skip opening quote */
    unsigned char *start = p;
    while (*p && *p != '"') p++;
    int len = (int)(p - start);
    if (*p == '"') p++;

    uint16_t idx = bc_add_constant_string(cs, start, len);
    bc_emit_byte(cs, OP_PUSH_STR);
    bc_emit_u16(cs, idx);
    *pp = p;
}

/* ------------------------------------------------------------------ */
/*  Skip balanced parentheses in tokenized stream                      */
/* ------------------------------------------------------------------ */

static void skip_balanced_parens(unsigned char **pp) {
    unsigned char *p = *pp;
    /* The function token (e.g. "Len(") already consumed the opening '(',
     * so p may point to the first argument, not '('.  Start depth at 1
     * if we don't see '('. */
    int depth;
    if (*p == '(') {
        depth = 0;
    } else {
        depth = 1;  /* opening '(' was part of the token name */
    }
    while (*p) {
        if (*p == '(')      depth++;
        else if (*p == ')') { depth--; if (depth <= 0) { p++; break; } }
        else if (*p == '"') {
            p++;
            while (*p && *p != '"') p++;
            if (*p == '"') { p++; continue; }
            break;
        }
        p++;
    }
    *pp = p;
}

/* ------------------------------------------------------------------ */
/*  Native function compilation                                        */
/*                                                                     */
/*  For common built-in functions, compile arguments as bytecode       */
/*  expressions and emit a native opcode instead of bridging.          */
/*  Returns 1 if handled natively, 0 if the VM should reject it.      */
/* ------------------------------------------------------------------ */

/* External function declarations from AllCommands.h */
extern void fun_len(void);
extern void fun_left(void);
extern void fun_right(void);
extern void fun_mid(void);
extern void fun_ucase(void);
extern void fun_lcase(void);
extern void fun_val(void);
extern void fun_str(void);
extern void fun_chr(void);
extern void fun_asc(void);
extern void fun_instr(void);
extern void fun_sin(void);
extern void fun_cos(void);
extern void fun_tan(void);
extern void fun_atn(void);
extern void fun_asin(void);
extern void fun_acos(void);
extern void fun_atan2(void);
extern void fun_sqr(void);
extern void fun_log(void);
extern void fun_exp(void);
extern void fun_abs(void);
extern void fun_sgn(void);
extern void fun_int(void);
extern void fun_fix(void);
extern void fun_cint(void);
extern void fun_rad(void);
extern void fun_deg(void);
extern void fun_pi(void);
extern void fun_max(void);
extern void fun_min(void);
extern void fun_hex(void);
extern void fun_oct(void);
extern void fun_bin(void);
extern void fun_space(void);
extern void fun_string(void);
extern void fun_field(void);
extern void fun_rnd(void);
extern void fun_inkey(void);
extern void fun_timer(void);
extern void fun_date(void);
extern void fun_time(void);
extern void fun_keydown(void);
extern void fun_info(void);
extern void fun_tilde(void);

/*
 * Try to compile a built-in function natively.
 * p points to the first argument (after the function token and its '(').
 * If this function has a native opcode, compile the args and emit the opcode.
 * Returns the return type (T_INT/T_STR/etc) or 0 if not handled natively.
 */
static uint8_t try_compile_native_fun(BCCompiler *cs, void (*fptr)(void),
                                       unsigned char **pp) {
    unsigned char *p = *pp;

    if (fptr == fun_len) {
        /* LEN(str$) -> push str, OP_STR_LEN */
        bc_compile_expression(cs, &p);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_STR_LEN);
        *pp = p;
        return T_INT;
    }

    if (fptr == fun_left) {
        /* LEFT$(str$, n%) -> push str, push n, OP_STR_LEFT */
        bc_compile_expression(cs, &p);
        if (*p == ',') p++;
        bc_compile_expression(cs, &p);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_STR_LEFT);
        *pp = p;
        return T_STR;
    }

    if (fptr == fun_right) {
        /* RIGHT$(str$, n%) -> push str, push n, OP_STR_RIGHT */
        bc_compile_expression(cs, &p);
        if (*p == ',') p++;
        bc_compile_expression(cs, &p);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_STR_RIGHT);
        *pp = p;
        return T_STR;
    }

    if (fptr == fun_mid) {
        /* MID$(str$, start [, len]) -> push str, push start, [push len], OP */
        bc_compile_expression(cs, &p);
        if (*p == ',') p++;
        bc_compile_expression(cs, &p);
        while (*p == ' ') p++;
        if (*p == ',') {
            p++;
            bc_compile_expression(cs, &p);
            if (*p == ')') p++;
            bc_emit_byte(cs, OP_STR_MID3);
        } else {
            if (*p == ')') p++;
            bc_emit_byte(cs, OP_STR_MID2);
        }
        *pp = p;
        return T_STR;
    }

    if (fptr == fun_ucase) {
        /* UCASE$(str$) -> push str, OP_STR_UCASE */
        bc_compile_expression(cs, &p);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_STR_UCASE);
        *pp = p;
        return T_STR;
    }

    if (fptr == fun_lcase) {
        /* LCASE$(str$) -> push str, OP_STR_LCASE */
        bc_compile_expression(cs, &p);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_STR_LCASE);
        *pp = p;
        return T_STR;
    }

    /* ---- Additional string functions ---- */

    if (fptr == fun_val) {
        /* VAL(str$) -> float */
        bc_compile_expression(cs, &p);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_STR_VAL);
        *pp = p;
        return T_NBR;
    }

    if (fptr == fun_str) {
        /* STR$(n) -> str$ */
        bc_compile_expression(cs, &p);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_STR_STR);
        *pp = p;
        return T_STR;
    }

    if (fptr == fun_chr) {
        /* CHR$(n%) -> str$ */
        bc_compile_expression(cs, &p);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_STR_CHR);
        *pp = p;
        return T_STR;
    }

    if (fptr == fun_asc) {
        /* ASC(str$) -> int */
        bc_compile_expression(cs, &p);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_STR_ASC);
        *pp = p;
        return T_INT;
    }

    if (fptr == fun_hex) {
        /* HEX$(n%) -> str$ */
        bc_compile_expression(cs, &p);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_STR_HEX);
        *pp = p;
        return T_STR;
    }

    if (fptr == fun_oct) {
        /* OCT$(n%) -> str$ */
        bc_compile_expression(cs, &p);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_STR_OCT);
        *pp = p;
        return T_STR;
    }

    if (fptr == fun_bin) {
        /* BIN$(n%) -> str$ */
        bc_compile_expression(cs, &p);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_STR_BIN);
        *pp = p;
        return T_STR;
    }

    if (fptr == fun_instr) {
        /* INSTR([start%,] haystack$, needle$ [, matchlen])
         * Count commas at this nesting level to determine arg count.
         * Only handle natively:
         *   2 args (1 comma): INSTR(haystack$, needle$)
         *   3 args (2 commas) with numeric first arg: INSTR(start%, haystack$, needle$)
         * All other forms (regex with matchlen) are not implemented yet. */
        int comma_count = 0;
        {
            unsigned char *scan = p;
            int depth = 1;
            while (*scan && depth > 0) {
                if (*scan == '(') depth++;
                else if (*scan == ')') { depth--; if (depth == 0) break; }
                else if (*scan == ',' && depth == 1) comma_count++;
                else if (*scan == '"') { scan++; while (*scan && *scan != '"') scan++; }
                scan++;
            }
        }
        /* 4 args (3 commas) is always regex form. */
        if (comma_count > 2) return 0;
        if (comma_count == 1) {
            /* 2-arg: INSTR(haystack$, needle$) */
            bc_compile_expression(cs, &p);  /* haystack$ */
            skipspace(p); if (*p == ',') p++;
            bc_compile_expression(cs, &p);  /* needle$ */
            skipspace(p); if (*p == ')') p++;
            bc_emit_byte(cs, OP_STR_INSTR);
            bc_emit_byte(cs, 2);
            *pp = p;
            return T_INT;
        }
        /* comma_count == 2: could be INSTR(start%, haystack$, needle$) or
         * INSTR(haystack$, regex$, matchlen).
         * Compile first arg speculatively; rewind if it is the unsupported regex form. */
        uint32_t saved_code_len = cs->code_len;
        uint8_t arg1_type = bc_compile_expression(cs, &p);
        skipspace(p); if (*p == ',') p++;
        if (arg1_type & T_STR) {
            /* String first arg + 3 args = regex form. */
            cs->code_len = saved_code_len;
            return 0;
        }
        /* Numeric first arg: INSTR(start%, haystack$, needle$) */
        if (arg1_type == T_NBR) bc_emit_byte(cs, OP_CVT_F2I);
        bc_compile_expression(cs, &p);  /* haystack$ */
        skipspace(p); if (*p == ',') p++;
        bc_compile_expression(cs, &p);  /* needle$ */
        skipspace(p); if (*p == ')') p++;
        bc_emit_byte(cs, OP_STR_INSTR);
        bc_emit_byte(cs, 3);
        *pp = p;
        return T_INT;
    }

    if (fptr == fun_space) {
        /* SPACE$(n%) -> str$ */
        uint8_t at = bc_compile_expression(cs, &p);
        if (at == T_NBR) bc_emit_byte(cs, OP_CVT_F2I);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_STR_SPACE);
        *pp = p;
        return T_STR;
    }

    if (fptr == fun_string) {
        /* STRING$(n%, char) -> str$  —  char can be int or string */
        uint8_t at1 = bc_compile_expression(cs, &p);  /* count */
        if (at1 == T_NBR) bc_emit_byte(cs, OP_CVT_F2I);
        skipspace(p); if (*p == ',') p++;
        uint8_t at2 = bc_compile_expression(cs, &p);  /* char value */
        if (at2 == T_STR) bc_emit_byte(cs, OP_STR_ASC);  /* extract first char as int */
        else if (at2 == T_NBR) bc_emit_byte(cs, OP_CVT_F2I);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_STR_STRING);
        *pp = p;
        return T_STR;
    }

    if (fptr == fun_field) {
        /* FIELD$(source$, field%, delims$) */
        bc_compile_expression(cs, &p);
        skipspace(p); if (*p == ',') p++;
        uint8_t at = bc_compile_expression(cs, &p);
        if (at == T_NBR) bc_emit_byte(cs, OP_CVT_F2I);
        skipspace(p); if (*p == ',') p++;
        bc_compile_expression(cs, &p);
        skipspace(p); if (*p == ')') p++;
        bc_emit_byte(cs, OP_STR_FIELD3);
        *pp = p;
        return T_STR;
    }

    if (fptr == fun_rgb) {
        unsigned char *scan = p;
        unsigned char *arg_end = p;
        int comma_count = 0;
        int depth = 1;

        while (*scan && depth > 0) {
            if (*scan == '(') depth++;
            else if (*scan == ')') {
                depth--;
                if (depth == 0) {
                    arg_end = scan;
                    break;
                }
            } else if (*scan == ',' && depth == 1) {
                comma_count++;
            } else if (*scan == '"') {
                scan++;
                while (*scan && *scan != '"') scan++;
            }
            scan++;
        }

        if (*scan != ')') return 0;

        if (comma_count == 0) {
            unsigned char *start = p;
            unsigned char *end = arg_end;
            int color = -1;
            while (start < end && *start == ' ') start++;
            while (end > start && end[-1] == ' ') end--;
#define RGB_NAME(name, value) \
            if (end - start == (int)strlen(name) && strncasecmp((const char *)start, name, strlen(name)) == 0) color = value;
            RGB_NAME("WHITE", WHITE)
            else RGB_NAME("YELLOW", YELLOW)
            else RGB_NAME("LILAC", LILAC)
            else RGB_NAME("BROWN", BROWN)
            else RGB_NAME("FUCHSIA", FUCHSIA)
            else RGB_NAME("RUST", RUST)
            else RGB_NAME("MAGENTA", MAGENTA)
            else RGB_NAME("RED", RED)
            else RGB_NAME("CYAN", CYAN)
            else RGB_NAME("GREEN", GREEN)
            else RGB_NAME("CERULEAN", CERULEAN)
            else RGB_NAME("MIDGREEN", MIDGREEN)
            else RGB_NAME("COBALT", COBALT)
            else RGB_NAME("MYRTLE", MYRTLE)
            else RGB_NAME("BLUE", BLUE)
            else RGB_NAME("BLACK", BLACK)
            else RGB_NAME("GRAY", GRAY)
            else RGB_NAME("GREY", GRAY)
            else RGB_NAME("LIGHTGRAY", LITEGRAY)
            else RGB_NAME("LIGHTGREY", LITEGRAY)
            else RGB_NAME("ORANGE", ORANGE)
            else RGB_NAME("PINK", PINK)
            else RGB_NAME("GOLD", GOLD)
            else RGB_NAME("SALMON", SALMON)
            else RGB_NAME("BEIGE", BEIGE)
#undef RGB_NAME
            if (color < 0) return 0;
            bc_emit_byte(cs, OP_PUSH_INT);
            bc_emit_i64(cs, color);
            p = scan + 1;
            *pp = p;
            return T_INT;
        }

        if (comma_count == 2) {
            uint8_t at;
            at = bc_compile_expression(cs, &p);
            if (at == T_NBR) bc_emit_byte(cs, OP_CVT_F2I);
            skipspace(p); if (*p == ',') p++;
            at = bc_compile_expression(cs, &p);
            if (at == T_NBR) bc_emit_byte(cs, OP_CVT_F2I);
            skipspace(p); if (*p == ',') p++;
            at = bc_compile_expression(cs, &p);
            if (at == T_NBR) bc_emit_byte(cs, OP_CVT_F2I);
            if (*p == ')') p++;
            bc_emit_byte(cs, OP_RGB);
            *pp = p;
            return T_INT;
        }
    }

    if (fptr == fun_keydown) {
        uint8_t at = bc_compile_expression(cs, &p);
        if (at == T_NBR) bc_emit_byte(cs, OP_CVT_F2I);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_KEYDOWN);
        *pp = p;
        return T_INT;
    }

    if (fptr == fun_info) {
        unsigned char *start = p;
        unsigned char *end = p;
        while (*end && *end != ')') end++;
        if (*end != ')') return 0;
        while (start < end && *start == ' ') start++;
        while (end > start && end[-1] == ' ') end--;
        if ((end - start) == 4 && strncasecmp((const char *)start, "HRES", 4) == 0) {
            bc_emit_byte(cs, OP_MM_HRES);
            *pp = end + 1;
            return T_INT;
        }
        if ((end - start) == 4 && strncasecmp((const char *)start, "VRES", 4) == 0) {
            bc_emit_byte(cs, OP_MM_VRES);
            *pp = end + 1;
            return T_INT;
        }
        return 0;
    }

    /* ---- Math functions (single-arg, float->float) ---- */

    /* ---- Math: single-arg float functions (auto-convert int to float) ---- */

#define NATIVE_FLOAT_FUN(fun_ptr, opcode) \
    if (fptr == fun_ptr) { \
        uint8_t at = bc_compile_expression(cs, &p); \
        if (at == T_INT) bc_emit_byte(cs, OP_CVT_I2F); \
        if (*p == ')') p++; \
        bc_emit_byte(cs, opcode); \
        *pp = p; \
        return T_NBR; \
    }

    NATIVE_FLOAT_FUN(fun_sin, OP_MATH_SIN)
    NATIVE_FLOAT_FUN(fun_cos, OP_MATH_COS)
    NATIVE_FLOAT_FUN(fun_tan, OP_MATH_TAN)
    NATIVE_FLOAT_FUN(fun_atn, OP_MATH_ATN)
    NATIVE_FLOAT_FUN(fun_asin, OP_MATH_ASIN)
    NATIVE_FLOAT_FUN(fun_acos, OP_MATH_ACOS)
    NATIVE_FLOAT_FUN(fun_sqr, OP_MATH_SQR)
    NATIVE_FLOAT_FUN(fun_log, OP_MATH_LOG)
    NATIVE_FLOAT_FUN(fun_exp, OP_MATH_EXP)
    NATIVE_FLOAT_FUN(fun_rad, OP_MATH_RAD)
    NATIVE_FLOAT_FUN(fun_deg, OP_MATH_DEG)

#undef NATIVE_FLOAT_FUN

    if (fptr == fun_atan2) {
        uint8_t at1 = bc_compile_expression(cs, &p);
        if (at1 == T_INT) bc_emit_byte(cs, OP_CVT_I2F);
        skipspace(p); if (*p == ',') p++;
        uint8_t at2 = bc_compile_expression(cs, &p);
        if (at2 == T_INT) bc_emit_byte(cs, OP_CVT_I2F);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_MATH_ATAN2);
        *pp = p;
        return T_NBR;
    }

    if (fptr == fun_tilde) {
        skipspace(p);
        if (*p == (unsigned char)('A' + MMHRES)) {
            p++;
            skipspace(p); if (*p == ')') p++;
            bc_emit_byte(cs, OP_MM_HRES);
            *pp = p;
            return T_INT;
        }
        if (*p == (unsigned char)('A' + MMVRES)) {
            p++;
            skipspace(p); if (*p == ')') p++;
            bc_emit_byte(cs, OP_MM_VRES);
            *pp = p;
            return T_INT;
        }
        return 0;
    }

    /* INT/FIX/CINT: take float, return float or int */
    if (fptr == fun_int) {
        uint8_t at = bc_compile_expression(cs, &p);
        if (at == T_INT) bc_emit_byte(cs, OP_CVT_I2F);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_MATH_INT);
        *pp = p;
        return T_NBR;
    }

    if (fptr == fun_fix) {
        uint8_t at = bc_compile_expression(cs, &p);
        if (at == T_INT) bc_emit_byte(cs, OP_CVT_I2F);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_MATH_FIX);
        *pp = p;
        return T_INT;
    }

    if (fptr == fun_cint) {
        uint8_t at = bc_compile_expression(cs, &p);
        if (at == T_INT) bc_emit_byte(cs, OP_CVT_I2F);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_MATH_CINT);
        *pp = p;
        return T_INT;
    }

    /* ABS: preserves input type (int->int, float->float) */
    if (fptr == fun_abs) {
        uint8_t at = bc_compile_expression(cs, &p);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_MATH_ABS);
        *pp = p;
        return at;
    }

    /* SGN: returns int regardless of input type */
    if (fptr == fun_sgn) {
        bc_compile_expression(cs, &p);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_MATH_SGN);
        *pp = p;
        return T_INT;
    }

    /* MAX/MIN: two-arg float functions */
    if (fptr == fun_max) {
        uint8_t at1 = bc_compile_expression(cs, &p);
        if (at1 == T_INT) bc_emit_byte(cs, OP_CVT_I2F);
        if (*p == ',') p++;
        uint8_t at2 = bc_compile_expression(cs, &p);
        if (at2 == T_INT) bc_emit_byte(cs, OP_CVT_I2F);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_MATH_MAX);
        *pp = p;
        return T_NBR;
    }

    if (fptr == fun_min) {
        uint8_t at1 = bc_compile_expression(cs, &p);
        if (at1 == T_INT) bc_emit_byte(cs, OP_CVT_I2F);
        if (*p == ',') p++;
        uint8_t at2 = bc_compile_expression(cs, &p);
        if (at2 == T_INT) bc_emit_byte(cs, OP_CVT_I2F);
        if (*p == ')') p++;
        bc_emit_byte(cs, OP_MATH_MIN);
        *pp = p;
        return T_NBR;
    }

    return 0;  /* not handled natively */
}

/* ------------------------------------------------------------------ */
/*  Emit a binary operator opcode                                      */
/*                                                                     */
/*  The caller has already inserted any necessary CVT instructions so  */
/*  ltype and rtype should match for ops that need matching types.     */
/*  Returns the result type.                                           */
/* ------------------------------------------------------------------ */

static uint8_t emit_binary_op(BCCompiler *cs, void (*fptr)(void),
                              uint8_t ltype, uint8_t rtype) {
    /* Arithmetic */
    if (fptr == op_add) {
        if (ltype == T_STR && rtype == T_STR) {
            bc_emit_byte(cs, OP_ADD_S); return T_STR;
        }
        if (ltype == T_NBR || rtype == T_NBR) {
            bc_emit_byte(cs, OP_ADD_F); return T_NBR;
        }
        bc_emit_byte(cs, OP_ADD_I); return T_INT;
    }
    if (fptr == op_subtract) {
        if (ltype == T_NBR || rtype == T_NBR) {
            bc_emit_byte(cs, OP_SUB_F); return T_NBR;
        }
        bc_emit_byte(cs, OP_SUB_I); return T_INT;
    }
    if (fptr == op_mul) {
        if (ltype == T_NBR || rtype == T_NBR) {
            bc_emit_byte(cs, OP_MUL_F); return T_NBR;
        }
        bc_emit_byte(cs, OP_MUL_I); return T_INT;
    }
    if (fptr == op_div)    { bc_emit_byte(cs, OP_DIV_F);  return T_NBR; }
    if (fptr == op_divint) { bc_emit_byte(cs, OP_IDIV_I); return T_INT; }
    if (fptr == op_mod) {
        if (ltype == T_NBR || rtype == T_NBR) {
            bc_emit_byte(cs, OP_MOD_F); return T_NBR;
        }
        bc_emit_byte(cs, OP_MOD_I); return T_INT;
    }
    if (fptr == op_exp)    { bc_emit_byte(cs, OP_POW_F);  return T_NBR; }

    /* Comparisons -- result is always T_INT */
    if (fptr == op_equal) {
        if (ltype == T_STR) { bc_emit_byte(cs, OP_EQ_S); return T_INT; }
        if (ltype == T_NBR) { bc_emit_byte(cs, OP_EQ_F); return T_INT; }
        bc_emit_byte(cs, OP_EQ_I); return T_INT;
    }
    if (fptr == op_ne) {
        if (ltype == T_STR) { bc_emit_byte(cs, OP_NE_S); return T_INT; }
        if (ltype == T_NBR) { bc_emit_byte(cs, OP_NE_F); return T_INT; }
        bc_emit_byte(cs, OP_NE_I); return T_INT;
    }
    if (fptr == op_lt) {
        if (ltype == T_STR) { bc_emit_byte(cs, OP_LT_S); return T_INT; }
        if (ltype == T_NBR) { bc_emit_byte(cs, OP_LT_F); return T_INT; }
        bc_emit_byte(cs, OP_LT_I); return T_INT;
    }
    if (fptr == op_gt) {
        if (ltype == T_STR) { bc_emit_byte(cs, OP_GT_S); return T_INT; }
        if (ltype == T_NBR) { bc_emit_byte(cs, OP_GT_F); return T_INT; }
        bc_emit_byte(cs, OP_GT_I); return T_INT;
    }
    if (fptr == op_lte) {
        if (ltype == T_STR) { bc_emit_byte(cs, OP_LE_S); return T_INT; }
        if (ltype == T_NBR) { bc_emit_byte(cs, OP_LE_F); return T_INT; }
        bc_emit_byte(cs, OP_LE_I); return T_INT;
    }
    if (fptr == op_gte) {
        if (ltype == T_STR) { bc_emit_byte(cs, OP_GE_S); return T_INT; }
        if (ltype == T_NBR) { bc_emit_byte(cs, OP_GE_F); return T_INT; }
        bc_emit_byte(cs, OP_GE_I); return T_INT;
    }

    /* Bitwise / logical -- integer only */
    if (fptr == op_and) { bc_emit_byte(cs, OP_AND); return T_INT; }
    if (fptr == op_or)  { bc_emit_byte(cs, OP_OR);  return T_INT; }
    if (fptr == op_xor) { bc_emit_byte(cs, OP_XOR); return T_INT; }

    /* Shifts */
    if (fptr == op_shiftleft)  { bc_emit_byte(cs, OP_SHL); return T_INT; }
    if (fptr == op_shiftright) { bc_emit_byte(cs, OP_SHR); return T_INT; }

    bc_set_error(cs, "Unknown binary operator");
    return ltype;
}

/* ------------------------------------------------------------------ */
/*  Emit a unary operator opcode                                       */
/* ------------------------------------------------------------------ */

static uint8_t emit_unary_op(BCCompiler *cs, void (*fptr)(void),
                             uint8_t operand_type) {
    if (fptr == op_subtract) {
        if (operand_type == T_NBR) {
            bc_emit_byte(cs, OP_NEG_F); return T_NBR;
        }
        bc_emit_byte(cs, OP_NEG_I); return T_INT;
    }
    if (fptr == op_not) {
        if (operand_type == T_NBR) bc_emit_byte(cs, OP_CVT_F2I);
        bc_emit_byte(cs, OP_NOT); return T_INT;
    }
    if (fptr == op_inv) {
        if (operand_type == T_NBR) bc_emit_byte(cs, OP_CVT_F2I);
        bc_emit_byte(cs, OP_INV); return T_INT;
    }
    if (fptr == op_add) {
        return operand_type;  /* unary plus is a no-op */
    }
    bc_set_error(cs, "Unknown unary operator");
    return operand_type;
}

/* ------------------------------------------------------------------ */
/*  Bytecode insertion for type promotion                              */
/*                                                                     */
/*  Insert one byte at position `pos`, shifting everything after it    */
/*  forward.  Also updates code_end in the type stack so later         */
/*  insertions target the right position.                              */
/* ------------------------------------------------------------------ */

static void code_insert_byte(BCCompiler *cs, uint32_t pos, uint8_t b,
                             TypeEntry *tstack, int tsp) {
    if (cs->code_len + 1 > BC_MAX_CODE) {
        bc_set_error(cs, "Bytecode overflow during insert");
        return;
    }
    memmove(&cs->code[pos + 1], &cs->code[pos], cs->code_len - pos);
    cs->code[pos] = b;
    cs->code_len++;

    for (int i = 0; i <= tsp; i++) {
        if (tstack[i].code_end >= pos)
            tstack[i].code_end++;
    }
}

/* ------------------------------------------------------------------ */
/*  Pop-and-emit helper: pops an operator and applies type promotion   */
/* ------------------------------------------------------------------ */

static void pop_and_emit(BCCompiler *cs, OpEntry *op,
                         TypeEntry *type_stack, int *ptsp) {
    int tsp = *ptsp;

    if (op->is_unary) {
        if (tsp < 0) {
            bc_set_error(cs, "Type stack underflow (unary)");
            return;
        }
        uint8_t t = emit_unary_op(cs, op->fptr, type_stack[tsp].type);
        type_stack[tsp].type = t;
        type_stack[tsp].code_end = cs->code_len;
    } else {
        if (tsp < 1) {
            bc_set_error(cs, "Type stack underflow (binary)");
            return;
        }
        uint8_t rtype = type_stack[tsp].type;
        tsp--;
        *ptsp = tsp;
        uint8_t ltype = type_stack[tsp].type;
        uint32_t left_end = type_stack[tsp].code_end;

        /* Type promotion: ensure matching numeric types */
        uint8_t eff_l = ltype, eff_r = rtype;

        /* Power (^) and float division (/) always need float operands */
        int needs_float = (op->fptr == op_exp || op->fptr == op_div);
        /* Integer division (\) always truncates integer operands */
        int needs_int = (op->fptr == op_divint);

        if (needs_int) {
            /* Coerce both sides to INT for OP_IDIV_I */
            if (rtype == T_NBR) {
                bc_emit_byte(cs, OP_CVT_F2I);
                eff_r = T_INT;
            }
            if (ltype == T_NBR) {
                code_insert_byte(cs, left_end, OP_CVT_F2I,
                                 type_stack, tsp);
                eff_l = T_INT;
            }
        } else if (needs_float && ltype == T_INT && rtype == T_INT) {
            /* Both int but need float: convert both */
            bc_emit_byte(cs, OP_CVT_I2F);
            eff_r = T_NBR;
            code_insert_byte(cs, left_end, OP_CVT_I2F,
                             type_stack, tsp);
            eff_l = T_NBR;
        } else if (ltype != rtype && ltype != T_STR && rtype != T_STR) {
            if (rtype == T_INT && ltype == T_NBR) {
                /* Right (TOS) is INT -- convert TOS to FLOAT */
                bc_emit_byte(cs, OP_CVT_I2F);
                eff_r = T_NBR;
            } else if (ltype == T_INT && rtype == T_NBR) {
                /* Left (below TOS) is INT -- insert CVT after left's code */
                code_insert_byte(cs, left_end, OP_CVT_I2F,
                                 type_stack, tsp);
                eff_l = T_NBR;
            }
        }
        uint8_t result = emit_binary_op(cs, op->fptr, eff_l, eff_r);
        type_stack[tsp].type = result;
        type_stack[tsp].code_end = cs->code_len;
    }
}

/* ------------------------------------------------------------------ */
/*  Should we pop the top operator before pushing a new one?           */
/*                                                                     */
/*  MMBasic: lower precedence NUMBER = tighter binding.                */
/*  Standard shunting-yard: pop when top has higher priority           */
/*  (= lower precedence number).                                      */
/*  For left-assoc: pop if top_prec <= new_prec.                      */
/*  For right-assoc (^): pop if top_prec < new_prec.                  */
/*  Unary ops (precedence -1) always have highest priority.            */
/* ------------------------------------------------------------------ */

static int should_pop(OpEntry *top, int new_prec, int new_right_assoc) {
    if (top->token == '(') return 0;
    int tp = top->precedence;
    if (new_right_assoc)
        return tp < new_prec;   /* strictly higher priority */
    else
        return tp <= new_prec;  /* higher or equal priority */
}

/* ------------------------------------------------------------------ */
/*  Main expression compiler                                           */
/* ------------------------------------------------------------------ */

uint8_t bc_compile_expression(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp;
    OpEntry   op_stack[OP_STACK_MAX];
    int       osp = -1;
    TypeEntry type_stack[TYPE_STACK_MAX];
    int       tsp = -1;
    int       paren_depth = 0;
    int       expect_value = 1;

    while (*p == ' ') p++;

    while (!cs->has_error) {
        while (*p == ' ') p++;
        if (is_expr_end(p, paren_depth)) break;

        /* ---- Open parenthesis ---- */
        if (*p == '(' && expect_value) {
            if (++osp >= OP_STACK_MAX) {
                bc_set_error(cs, "Operator stack overflow"); break;
            }
            op_stack[osp].token = '(';
            op_stack[osp].precedence = 99;
            op_stack[osp].is_unary = 0;
            op_stack[osp].fptr = NULL;
            paren_depth++;
            p++;
            expect_value = 1;
            continue;
        }

        /* ---- Close parenthesis ---- */
        if (*p == ')') {
            while (osp >= 0 && op_stack[osp].token != '(') {
                OpEntry op = op_stack[osp--];
                pop_and_emit(cs, &op, type_stack, &tsp);
                if (cs->has_error) break;
            }
            if (cs->has_error) break;
            if (osp < 0) break;  /* unmatched -- expression boundary */
            osp--;  /* pop the '(' */
            paren_depth--;
            p++;
            expect_value = 0;
            continue;
        }

        /* ---- Unary operators (at start, after '(' or after binary op) ---- */
        if (expect_value && *p >= C_BASETOKEN && is_operator_token(*p)) {
            void (*fptr)(void) = get_token_fptr(*p);
            if (fptr == op_subtract || fptr == op_add ||
                fptr == op_not || fptr == op_inv) {
                if (++osp >= OP_STACK_MAX) {
                    bc_set_error(cs, "Operator stack overflow"); break;
                }
                op_stack[osp].token = *p;
                op_stack[osp].precedence = -1; /* highest priority */
                op_stack[osp].is_unary = 1;
                op_stack[osp].fptr = fptr;
                p++;
                expect_value = 1;
                continue;
            }
        }

        /* ---- Number literal ---- */
        if (expect_value && ((*p >= '0' && *p <= '9') ||
                             *p == '.' || *p == '&')) {
            uint8_t ntype = T_INT;
            parse_number(cs, &p, &ntype);
            if (++tsp >= TYPE_STACK_MAX) {
                bc_set_error(cs, "Type stack overflow"); break;
            }
            type_stack[tsp].type = ntype;
            type_stack[tsp].code_end = cs->code_len;
            expect_value = 0;
            continue;
        }

        /* ---- String literal ---- */
        if (expect_value && *p == '"') {
            parse_string(cs, &p);
            if (++tsp >= TYPE_STACK_MAX) {
                bc_set_error(cs, "Type stack overflow"); break;
            }
            type_stack[tsp].type = T_STR;
            type_stack[tsp].code_end = cs->code_len;
            expect_value = 0;
            continue;
        }

        /* ---- Built-in function token (T_FUN / T_FNA) ---- */
        if (expect_value && *p >= C_BASETOKEN && is_function_token(*p)) {
            unsigned char fun_tok = *p;
            uint8_t ret_type = get_function_return_type(fun_tok);
            int has_args = (tokentype(fun_tok) & T_FUN) != 0;

            /* Try native compilation first (compiles args as expressions) */
            if (has_args) {
                void (*fptr)(void) = get_token_fptr(fun_tok);
                p++;  /* skip function token (the '(' is part of the token) */
                uint8_t native_ret = try_compile_native_fun(cs, fptr, &p);
                if (native_ret) {
                    ret_type = native_ret;
                    goto fun_done;
                }
                skip_balanced_parens(&p);
                bc_set_error(cs, "Unsupported VM function: %s", tokenname(fun_tok));
                break;
            } else {
                /* No-arg function (T_FNA) — check for native handling */
                void (*fptr)(void) = get_token_fptr(fun_tok);
                p++;
                if (fptr == fun_timer) {
                    bc_emit_byte(cs, OP_TIMER);
                    ret_type = T_NBR;
                    goto fun_done;
                }
                if (fptr == fun_pi) {
                    bc_emit_byte(cs, OP_MATH_PI);
                    ret_type = T_NBR;
                    goto fun_done;
                }
                if (fptr == fun_rnd) {
                    bc_emit_byte(cs, OP_RND);
                    ret_type = T_NBR;
                    goto fun_done;
                }
                if (fptr == fun_inkey) {
                    bc_emit_byte(cs, OP_STR_INKEY);
                    ret_type = T_STR;
                    goto fun_done;
                }
                if (fptr == fun_date) {
                    bc_emit_byte(cs, OP_STR_DATE);
                    ret_type = T_STR;
                    goto fun_done;
                }
                if (fptr == fun_time) {
                    bc_emit_byte(cs, OP_STR_TIME);
                    ret_type = T_STR;
                    goto fun_done;
                }
                bc_set_error(cs, "Unsupported VM function: %s", tokenname(fun_tok));
                break;
            }

        fun_done:
            if (++tsp >= TYPE_STACK_MAX) {
                bc_set_error(cs, "Type stack overflow"); break;
            }
            type_stack[tsp].type = ret_type;
            type_stack[tsp].code_end = cs->code_len;
            expect_value = 0;
            continue;
        }

        /* ---- Variable, array access, or user-defined function ---- */
        if (expect_value && isnamestart(*p)) {
            uint8_t var_type = 0;
            int is_array = 0;
            int has_suffix = 0;  /* was suffix in source text? */
            int name_len = bc_parse_varname(p, &var_type, &is_array);
            if (var_type != 0) has_suffix = 1;

            if (name_len == 0) {
                bc_set_error(cs, "Invalid variable name"); break;
            }
            if (var_type == 0) {
                /* No suffix — resolve locals first, then globals/functions. */
                int loc = bc_find_local(cs, (const char *)p, name_len);
                if (loc >= 0) {
                    var_type = cs->locals[loc].type;
                } else {
                    /* Check if it's an existing slot (e.g. CONST
                     * declared without a suffix, which defaults to T_NBR) */
                    uint16_t existing = bc_find_slot(cs, (const char *)p, name_len);
                    if (existing != 0xFFFF) {
                        var_type = cs->slots[existing].type;
                    } else {
                        /* Check if it's a user FUNCTION with AS type */
                        int sf_idx = bc_find_subfun(cs, (const char *)p, name_len);
                        if (sf_idx >= 0 && cs->subfuns[sf_idx].return_type != 0) {
                            var_type = cs->subfuns[sf_idx].return_type;
                        } else {
                            bc_set_error(cs,
                                "Variable must have explicit type suffix (%%/!/$)");
                            break;
                        }
                    }
                }
            }

            /* Followed by '(' -- array or function call */
            if (p[name_len] == '(' || is_array) {
                unsigned char *name_start = p;

                /* Check for user-defined FUNCTION (strip type suffix for lookup) */
                int sf_name_len = has_suffix ? name_len - 1 : name_len;
                int sf_idx = bc_find_subfun(cs, (const char *)p, sf_name_len);
                if (sf_idx >= 0 && cs->subfuns[sf_idx].return_type != 0) {
                    p += name_len;
                    if (*p != '(') {
                        bc_set_error(cs, "Expected '(' after function name");
                        break;
                    }
                    p++;  /* skip '(' */

                    int nargs = 0;
                    while (*p != ')' && *p != 0) {
                        while (*p == ' ') p++;
                        if (*p == ')') break;
                        bc_compile_expression(cs, &p);
                        if (cs->has_error) break;
                        nargs++;
                        while (*p == ' ') p++;
                        if (*p == ',') p++;
                    }
                    if (cs->has_error) break;
                    if (*p == ')') p++;

                    bc_emit_byte(cs, OP_CALL_FUN);
                    bc_emit_u16(cs, (uint16_t)sf_idx);
                    bc_emit_byte(cs, (uint8_t)nargs);

                    uint8_t ret = cs->subfuns[sf_idx].return_type;
                    if (++tsp >= TYPE_STACK_MAX) {
                        bc_set_error(cs, "Type stack overflow"); break;
                    }
                    type_stack[tsp].type = ret;
                    type_stack[tsp].code_end = cs->code_len;
                    expect_value = 0;
                    continue;
                }

                /* Array access */
                p += name_len;
                if (*p != '(') {
                    bc_set_error(cs, "Expected '(' for array access");
                    break;
                }
                p++;  /* skip '(' */

                int ndims = 0;
                while (*p != ')' && *p != 0) {
                    while (*p == ' ') p++;
                    if (*p == ')') break;
                    bc_compile_expression(cs, &p);
                    if (cs->has_error) break;
                    ndims++;
                    while (*p == ' ') p++;
                    if (*p == ',') p++;
                }
                if (cs->has_error) break;
                if (*p == ')') p++;

                int is_local = 0;
                uint16_t slot;
                int loc = bc_find_local(cs, (const char *)name_start,
                                        name_len);
                if (loc >= 0) {
                    slot = (uint16_t)loc;
                    is_local = 1;
                } else {
                    slot = bc_find_slot(cs, (const char *)name_start,
                                        name_len);
                    if (slot == 0xFFFF)
                        slot = bc_add_slot(cs, (const char *)name_start,
                                           name_len, var_type, 1);
                }

                uint8_t arr_op;
                if (is_local) {
                    arr_op = (var_type == T_INT) ? OP_LOAD_LOCAL_ARR_I :
                             (var_type == T_NBR) ? OP_LOAD_LOCAL_ARR_F :
                                                   OP_LOAD_LOCAL_ARR_S;
                } else {
                    arr_op = (var_type == T_INT) ? OP_LOAD_ARR_I :
                             (var_type == T_NBR) ? OP_LOAD_ARR_F :
                                                   OP_LOAD_ARR_S;
                }
                bc_emit_byte(cs, arr_op);
                bc_emit_u16(cs, slot);
                bc_emit_byte(cs, (uint8_t)ndims);

                if (++tsp >= TYPE_STACK_MAX) {
                    bc_set_error(cs, "Type stack overflow"); break;
                }
                type_stack[tsp].type = var_type;
                type_stack[tsp].code_end = cs->code_len;
                expect_value = 0;
                continue;
            }

            /* Plain scalar variable */
            int is_local = 0;
            uint16_t slot;
            int loc = bc_find_local(cs, (const char *)p, name_len);
            if (loc >= 0) {
                slot = (uint16_t)loc;
                is_local = 1;
            } else {
                slot = bc_find_slot(cs, (const char *)p, name_len);
                if (slot == 0xFFFF)
                    slot = bc_add_slot(cs, (const char *)p, name_len,
                                       var_type, 0);
            }
            bc_emit_load_var(cs, slot, var_type, is_local);
            p += name_len;

            if (++tsp >= TYPE_STACK_MAX) {
                bc_set_error(cs, "Type stack overflow"); break;
            }
            type_stack[tsp].type = var_type;
            type_stack[tsp].code_end = cs->code_len;
            expect_value = 0;
            continue;
        }

        /* ---- Binary operator ---- */
        if (!expect_value && *p >= C_BASETOKEN && is_operator_token(*p)) {
            unsigned char tok = *p;
            void (*fptr)(void) = get_token_fptr(tok);
            int prec = get_precedence(tok);
            int ra = is_right_assoc(fptr);

            /* Pop operators with higher or equal priority */
            while (osp >= 0 && should_pop(&op_stack[osp], prec, ra)) {
                OpEntry op = op_stack[osp--];
                pop_and_emit(cs, &op, type_stack, &tsp);
                if (cs->has_error) break;
            }
            if (cs->has_error) break;

            if (++osp >= OP_STACK_MAX) {
                bc_set_error(cs, "Operator stack overflow"); break;
            }
            op_stack[osp].token = tok;
            op_stack[osp].precedence = prec;
            op_stack[osp].is_unary = 0;
            op_stack[osp].fptr = fptr;
            p++;
            expect_value = 1;
            continue;
        }

        /* '(' after a value -- not part of this expression */
        if (!expect_value && *p == '(') break;

        if (expect_value) {
            bc_set_error(cs,
                "Expected value in expression at '%c' (0x%02X)",
                *p >= 0x20 ? *p : '?', *p);
            break;
        }

        /* Unknown token -- expression boundary */
        break;
    }

    /* Flush remaining operators */
    while (osp >= 0 && !cs->has_error) {
        OpEntry op = op_stack[osp--];
        if (op.token == '(') {
            bc_set_error(cs, "Unmatched '(' in expression");
            break;
        }
        pop_and_emit(cs, &op, type_stack, &tsp);
    }

    *pp = p;
    if (tsp < 0) {
        if (!cs->has_error) bc_set_error(cs, "Empty expression");
        return T_INT;
    }
    if (tsp > 0 && !cs->has_error)
        bc_set_error(cs, "Expression has %d extra values on stack", tsp);
    return type_stack[0].type;
}
