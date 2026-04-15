#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bc_compiler_internal.h"
#include "bc_source.h"
#include "MMBasic.h"
#include "Draw.h"
#include "vm_sys_pin.h"
#include "vm_sys_file.h"

typedef struct {
    int line_no;
} BCSourceFrontend;

#ifdef MMBASIC_HOST
int bc_opt_level = 1;
#else
int bc_opt_level = 1;
#endif

static uint8_t source_parse_expression(BCSourceFrontend *fe, BCCompiler *cs, const char **pp);
static void source_compile_statement(BCSourceFrontend *fe, BCCompiler *cs, const char *stmt);
static int source_compile_call_args(BCSourceFrontend *fe, BCCompiler *cs, const char **pp,
                                    int require_parens);
static int source_parse_array_indices(BCSourceFrontend *fe, BCCompiler *cs, const char **pp);
static void source_emit_int_conversion(BCCompiler *cs, uint8_t type);
static void source_emit_syscall(BCCompiler *cs, uint16_t sysid, uint8_t argc,
                                const uint8_t *aux, uint8_t auxlen);
static void source_emit_syscall_noaux(BCCompiler *cs, uint16_t sysid, uint8_t argc);

static void source_skip_space(const char **pp) {
    while (**pp == ' ' || **pp == '\t') (*pp)++;
}

static int source_keyword(const char **pp, const char *kw) {
    const char *p = *pp;
    unsigned char next;
    size_t len = strlen(kw);
    if (strncasecmp(p, kw, len) != 0) return 0;
    next = (unsigned char)p[len];
    if (isnamechar(next) || next == '$' || next == '%' || next == '!') return 0;
    *pp = p + len;
    return 1;
}

static int source_line_empty_or_comment(const char *p) {
    source_skip_space(&p);
    return *p == '\0' || *p == '\'' ||
           (strncasecmp(p, "REM", 3) == 0 && !isnamechar((unsigned char)p[3]));
}

static void source_statement_end(BCCompiler *cs, const char *p) {
    source_skip_space(&p);
    if (*p != '\0' && *p != '\'')
        bc_set_error(cs, "Unsupported source syntax near: %.24s", p);
}

static void source_insert_byte(BCCompiler *cs, uint32_t pos, uint8_t b) {
    if (cs->code_len >= BC_MAX_CODE) {
        bc_set_error(cs, "Bytecode overflow (%d bytes)", BC_MAX_CODE);
        return;
    }
    if (pos > cs->code_len) {
        bc_set_error(cs, "Internal source compiler error");
        return;
    }
    memmove(&cs->code[pos + 1], &cs->code[pos], cs->code_len - pos);
    cs->code[pos] = b;
    cs->code_len++;
}

static void source_delete_bytes(BCCompiler *cs, uint32_t pos, uint32_t count) {
    if (count == 0) return;
    if (pos + count > cs->code_len) {
        bc_set_error(cs, "Internal source compiler error");
        return;
    }
    memmove(&cs->code[pos], &cs->code[pos + count], cs->code_len - (pos + count));
    cs->code_len -= count;
}

static int source_power_of_two_bits_i64(int64_t v) {
    int bits = 0;
    if (v <= 0) return -1;
    while ((v & 1) == 0) {
        v >>= 1;
        bits++;
    }
    return (v == 1) ? bits : -1;
}

static int source_try_fuse_mulshr(BCCompiler *cs, uint8_t left, uint8_t right,
                                  uint32_t expr_start, uint32_t right_start) {
    uint32_t mul_pos;
    uint32_t mul_len;
    int64_t divisor;
    int bits;

    if (bc_opt_level < 1) return 0;
    if (left != T_INT || right != T_INT) return 0;
    if (right_start < 1 || right_start + 9 != cs->code_len) return 0;
    if (cs->code[right_start - 1] != OP_MUL_I) return 0;
    if (cs->code[right_start] != OP_PUSH_INT) return 0;

    memcpy(&divisor, &cs->code[right_start + 1], sizeof(divisor));
    bits = source_power_of_two_bits_i64(divisor);
    if (bits < 0) return 0;

    mul_pos = right_start - 1;
    mul_len = mul_pos - expr_start;
    source_delete_bytes(cs, mul_pos, 1);
    if (cs->has_error) return 0;

    divisor = (int64_t)bits;
    cs->code[mul_pos] = OP_PUSH_INT;
    memcpy(&cs->code[mul_pos + 1], &divisor, sizeof(divisor));
    if (mul_len == 6 &&
        cs->code[expr_start] == OP_LOAD_I &&
        cs->code[expr_start + 3] == OP_LOAD_I &&
        memcmp(&cs->code[expr_start + 1], &cs->code[expr_start + 4], 2) == 0) {
        source_delete_bytes(cs, expr_start + 3, 3);
        if (cs->has_error) return 0;
        bc_emit_byte(cs, OP_MATH_SQRSHR);
        return 1;
    }
    if (mul_len == 6 &&
        cs->code[expr_start] == OP_LOAD_LOCAL_I &&
        cs->code[expr_start + 3] == OP_LOAD_LOCAL_I &&
        memcmp(&cs->code[expr_start + 1], &cs->code[expr_start + 4], 2) == 0) {
        source_delete_bytes(cs, expr_start + 3, 3);
        if (cs->has_error) return 0;
        bc_emit_byte(cs, OP_MATH_SQRSHR);
        return 1;
    }
    bc_emit_byte(cs, OP_MATH_MULSHR);
    return 1;
}

static int source_is_same_simple_int_load(BCCompiler *cs,
                                          uint32_t start_a, uint32_t end_a,
                                          uint32_t start_b, uint32_t end_b) {
    uint32_t len_a = end_a - start_a;
    uint32_t len_b = end_b - start_b;

    if (len_a != len_b) return 0;
    if (len_a == 3 &&
        cs->code[start_a] == OP_LOAD_I &&
        cs->code[start_b] == OP_LOAD_I &&
        memcmp(&cs->code[start_a + 1], &cs->code[start_b + 1], 2) == 0)
        return 1;
    if (len_a == 3 &&
        cs->code[start_a] == OP_LOAD_LOCAL_I &&
        cs->code[start_b] == OP_LOAD_LOCAL_I &&
        memcmp(&cs->code[start_a + 1], &cs->code[start_b + 1], 2) == 0)
        return 1;
    return 0;
}

static int source_try_fuse_mulshradd(BCCompiler *cs, uint8_t left, uint8_t right,
                                     uint32_t right_start, char op) {
    if (bc_opt_level < 1) return 0;
    if (op != '+') return 0;
    if (left != T_INT || right != T_INT) return 0;
    if (right_start < 1) return 0;
    if (cs->code[right_start - 1] != OP_MATH_MULSHR)
        return 0;
    source_delete_bytes(cs, right_start - 1, 1);
    if (cs->has_error) return 0;
    bc_emit_byte(cs, OP_MATH_MULSHRADD);
    return 1;
}

static int source_try_fuse_mov_assignment(BCCompiler *cs, uint32_t expr_start,
                                          uint16_t dst_slot, int dst_is_local,
                                          uint8_t vtype, uint8_t etype) {
    uint32_t expr_len = cs->code_len - expr_start;
    uint16_t src_slot;
    uint8_t mov_kind;
    uint16_t src_raw;
    uint16_t dst_raw = dst_is_local ? (uint16_t)(dst_slot | 0x8000u) : dst_slot;
    int src_is_local;

    if (bc_opt_level < 1) return 0;
    if (expr_len != 3) return 0;
    (void)etype;

    if (vtype == T_INT) {
        mov_kind = BC_MOV_INT;
        if (cs->code[expr_start] == OP_LOAD_LOCAL_I) src_is_local = 1;
        else if (cs->code[expr_start] == OP_LOAD_I) src_is_local = 0;
        else return 0;
    } else if (vtype == T_NBR) {
        mov_kind = BC_MOV_FLT;
        if (cs->code[expr_start] == OP_LOAD_LOCAL_F) src_is_local = 1;
        else if (cs->code[expr_start] == OP_LOAD_F) src_is_local = 0;
        else return 0;
    } else if (vtype == T_STR) {
        mov_kind = BC_MOV_STR;
        if (cs->code[expr_start] == OP_LOAD_LOCAL_S) src_is_local = 1;
        else if (cs->code[expr_start] == OP_LOAD_S) src_is_local = 0;
        else return 0;
    } else {
        return 0;
    }

    memcpy(&src_slot, &cs->code[expr_start + 1], sizeof(src_slot));
    src_raw = src_is_local ? (uint16_t)(src_slot | 0x8000u) : src_slot;

    source_delete_bytes(cs, expr_start, expr_len);
    if (cs->has_error) return 0;
    bc_emit_byte(cs, OP_MOV_VAR);
    bc_emit_byte(cs, mov_kind);
    bc_emit_u16(cs, src_raw);
    bc_emit_u16(cs, dst_raw);
    return 1;
}

static uint8_t source_jcmp_relation(uint8_t compare_op, uint8_t branch_op, uint8_t *jcmp_op) {
    if (branch_op != OP_JZ && branch_op != OP_JNZ) return 0;
    switch (compare_op) {
        case OP_EQ_I: *jcmp_op = OP_JCMP_I; return (branch_op == OP_JNZ) ? BC_JCMP_EQ : BC_JCMP_NE;
        case OP_NE_I: *jcmp_op = OP_JCMP_I; return (branch_op == OP_JNZ) ? BC_JCMP_NE : BC_JCMP_EQ;
        case OP_LT_I: *jcmp_op = OP_JCMP_I; return (branch_op == OP_JNZ) ? BC_JCMP_LT : BC_JCMP_GE;
        case OP_GT_I: *jcmp_op = OP_JCMP_I; return (branch_op == OP_JNZ) ? BC_JCMP_GT : BC_JCMP_LE;
        case OP_LE_I: *jcmp_op = OP_JCMP_I; return (branch_op == OP_JNZ) ? BC_JCMP_LE : BC_JCMP_GT;
        case OP_GE_I: *jcmp_op = OP_JCMP_I; return (branch_op == OP_JNZ) ? BC_JCMP_GE : BC_JCMP_LT;
        case OP_EQ_F: *jcmp_op = OP_JCMP_F; return (branch_op == OP_JNZ) ? BC_JCMP_EQ : BC_JCMP_NE;
        case OP_NE_F: *jcmp_op = OP_JCMP_F; return (branch_op == OP_JNZ) ? BC_JCMP_NE : BC_JCMP_EQ;
        case OP_LT_F: *jcmp_op = OP_JCMP_F; return (branch_op == OP_JNZ) ? BC_JCMP_LT : BC_JCMP_GE;
        case OP_GT_F: *jcmp_op = OP_JCMP_F; return (branch_op == OP_JNZ) ? BC_JCMP_GT : BC_JCMP_LE;
        case OP_LE_F: *jcmp_op = OP_JCMP_F; return (branch_op == OP_JNZ) ? BC_JCMP_LE : BC_JCMP_GT;
        case OP_GE_F: *jcmp_op = OP_JCMP_F; return (branch_op == OP_JNZ) ? BC_JCMP_GE : BC_JCMP_LT;
        default:      return 0;
    }
}

static uint32_t source_emit_jmp_placeholder(BCCompiler *cs, uint8_t opcode) {
    uint8_t rel;
    uint8_t jcmp_op;
    if (bc_opt_level >= 1 && cs->code_len > 0 &&
        (rel = source_jcmp_relation(cs->code[cs->code_len - 1], opcode, &jcmp_op)) != 0) {
        source_delete_bytes(cs, cs->code_len - 1, 1);
        if (cs->has_error) return 0;
        bc_emit_byte(cs, jcmp_op);
        bc_emit_byte(cs, rel);
        uint32_t patch = cs->code_len;
        bc_emit_i16(cs, 0);
        return patch;
    }
    bc_emit_byte(cs, opcode);
    uint32_t patch = cs->code_len;
    bc_emit_i16(cs, 0);
    return patch;
}

static void source_patch_jmp_here(BCCompiler *cs, uint32_t patch_addr) {
    bc_patch_i16(cs, patch_addr, (int16_t)(cs->code_len - (patch_addr + 2)));
}

static void source_emit_rel_jump(BCCompiler *cs, uint8_t opcode, uint32_t target_addr) {
    uint8_t rel;
    uint8_t jcmp_op;
    if (bc_opt_level >= 1 && cs->code_len > 0 &&
        (rel = source_jcmp_relation(cs->code[cs->code_len - 1], opcode, &jcmp_op)) != 0) {
        source_delete_bytes(cs, cs->code_len - 1, 1);
        if (cs->has_error) return;
        bc_emit_byte(cs, jcmp_op);
        bc_emit_byte(cs, rel);
        bc_emit_i16(cs, (int16_t)(target_addr - (cs->code_len + 2)));
        return;
    }
    bc_emit_byte(cs, opcode);
    bc_emit_i16(cs, (int16_t)(target_addr - (cs->code_len + 2)));
}

static int source_parse_line_number(const char **pp) {
    const char *p = *pp;
    source_skip_space(&p);
    if (!isdigit((unsigned char)*p)) return -1;
    int num = 0;
    while (isdigit((unsigned char)*p)) {
        num = num * 10 + (*p - '0');
        p++;
    }
    *pp = p;
    return num;
}

static void source_emit_abs_jump(BCCompiler *cs, uint8_t opcode, int lineno) {
    bc_emit_byte(cs, opcode);
    uint32_t patch = cs->code_len;
    bc_emit_u32(cs, 0);
    uint32_t target = bc_linemap_lookup(cs, (uint16_t)lineno);
    if (target != 0xFFFFFFFF)
        bc_patch_u32(cs, patch, target);
    else
        bc_add_fixup_line(cs, patch, lineno, 4, 0);
}

static int source_get_or_create_subfun(BCCompiler *cs, const char *name,
                                       int name_len, uint8_t return_type) {
    int idx = bc_find_subfun(cs, name, name_len);
    if (idx >= 0) {
        cs->subfuns[idx].return_type = return_type;
        return idx;
    }
    if (cs->subfun_count >= BC_MAX_SUBFUNS) {
        bc_set_error(cs, "Too many SUB/FUNCTION definitions");
        return -1;
    }
    idx = cs->subfun_count++;
    int copy_len = name_len > MAXVARLEN ? MAXVARLEN : name_len;
    memcpy(cs->subfuns[idx].name, name, copy_len);
    cs->subfuns[idx].name[copy_len] = '\0';
    cs->subfuns[idx].return_type = return_type;
    return idx;
}

static int source_parse_varname(const char **pp, char *name, int *name_len, uint8_t *type) {
    const char *p = *pp;
    source_skip_space(&p);
    if (!isnamestart((unsigned char)*p)) return 0;

    int len = 0;
    while (isnamechar((unsigned char)p[len]) && len < MAXVARLEN) {
        name[len] = p[len];
        len++;
    }
    while (isnamechar((unsigned char)p[len])) len++;

    *type = 0;
    if (p[len] == '%' || p[len] == '!' || p[len] == '$') {
        *type = bc_type_from_suffix(p[len]);
        if (len < MAXVARLEN) name[len] = p[len];
        len++;
    }

    int copy_len = len > MAXVARLEN ? MAXVARLEN : len;
    name[copy_len] = '\0';
    *name_len = copy_len;
    *pp = p + len;
    return 1;
}

static uint16_t source_resolve_global(BCCompiler *cs, const char *name, int name_len,
                                      uint8_t type, int create) {
    uint16_t slot = bc_find_slot(cs, name, name_len);
    if (slot != 0xFFFF) return slot;
    if (!create) return 0xFFFF;
    return bc_add_slot(cs, name, name_len, type, 0);
}

static uint16_t source_resolve_var(BCCompiler *cs, const char *name, int name_len,
                                   uint8_t type, int create, int *is_local) {
    *is_local = 0;
    if (cs->current_subfun >= 0) {
        int loc = bc_find_local(cs, name, name_len);
        if (loc >= 0) {
            *is_local = 1;
            return (uint16_t)loc;
        }
    }
    return source_resolve_global(cs, name, name_len, type, create);
}

static uint8_t source_default_var_type(BCCompiler *cs, const char *name, int name_len) {
    if (cs->current_subfun >= 0) {
        int loc = bc_find_local(cs, name, name_len);
        if (loc >= 0) return cs->locals[loc].type;
    }
    uint16_t slot = bc_find_slot(cs, name, name_len);
    return slot == 0xFFFF ? T_NBR : cs->slots[slot].type;
}

static uint16_t source_alloc_hidden_slot(BCCompiler *cs, uint8_t type) {
    char buf[MAXVARLEN + 1];
    snprintf(buf, sizeof(buf), "#SRC_%u", (unsigned)cs->next_hidden_slot++);
    return bc_add_slot(cs, buf, (int)strlen(buf), type, 0);
}

/* Allocate a hidden local slot (for FOR limit/step inside SUB/FUNCTION) */
static uint16_t source_alloc_hidden_local(BCCompiler *cs, uint8_t type) {
    char buf[MAXVARLEN + 1];
    snprintf(buf, sizeof(buf), "#SRC_%u", (unsigned)cs->next_hidden_slot++);
    int idx = bc_add_local(cs, buf, (int)strlen(buf), type, 0);
    if (idx < 0) return 0;
    return (uint16_t)idx;
}

static int source_color_name(const char *start, const char *end, int *color) {
    while (start < end && (*start == ' ' || *start == '\t')) start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
#define SOURCE_RGB_NAME(name, value) \
    if (end - start == (int)strlen(name) && strncasecmp(start, name, strlen(name)) == 0) { \
        *color = (int)(value); \
        return 1; \
    }
    SOURCE_RGB_NAME("WHITE", WHITE)
    SOURCE_RGB_NAME("YELLOW", YELLOW)
    SOURCE_RGB_NAME("LILAC", LILAC)
    SOURCE_RGB_NAME("BROWN", BROWN)
    SOURCE_RGB_NAME("FUCHSIA", FUCHSIA)
    SOURCE_RGB_NAME("RUST", RUST)
    SOURCE_RGB_NAME("MAGENTA", MAGENTA)
    SOURCE_RGB_NAME("RED", RED)
    SOURCE_RGB_NAME("CYAN", CYAN)
    SOURCE_RGB_NAME("GREEN", GREEN)
    SOURCE_RGB_NAME("CERULEAN", CERULEAN)
    SOURCE_RGB_NAME("MIDGREEN", MIDGREEN)
    SOURCE_RGB_NAME("COBALT", COBALT)
    SOURCE_RGB_NAME("MYRTLE", MYRTLE)
    SOURCE_RGB_NAME("BLUE", BLUE)
    SOURCE_RGB_NAME("BLACK", BLACK)
    SOURCE_RGB_NAME("GRAY", GRAY)
    SOURCE_RGB_NAME("GREY", GRAY)
    SOURCE_RGB_NAME("LIGHTGRAY", LITEGRAY)
    SOURCE_RGB_NAME("LIGHTGREY", LITEGRAY)
    SOURCE_RGB_NAME("ORANGE", ORANGE)
    SOURCE_RGB_NAME("PINK", PINK)
    SOURCE_RGB_NAME("GOLD", GOLD)
    SOURCE_RGB_NAME("SALMON", SALMON)
    SOURCE_RGB_NAME("BEIGE", BEIGE)
#undef SOURCE_RGB_NAME
    return 0;
}

static int source_parse_setpin_mode(const char **pp, int *mode) {
    const char *p = *pp;
    source_skip_space(&p);
    if (source_keyword(&p, "OFF")) {
        *mode = VM_PIN_MODE_OFF;
    } else if (source_keyword(&p, "DIN")) {
        *mode = VM_PIN_MODE_DIN;
    } else if (source_keyword(&p, "DOUT")) {
        *mode = VM_PIN_MODE_DOUT;
    } else if (source_keyword(&p, "ARAW")) {
        *mode = VM_PIN_MODE_ARAW;
    } else if (source_keyword(&p, "PWM0A")) {
        *mode = VM_PIN_MODE_PWM0A;
    } else if (source_keyword(&p, "PWM0B")) {
        *mode = VM_PIN_MODE_PWM0B;
    } else if (source_keyword(&p, "PWM1A")) {
        *mode = VM_PIN_MODE_PWM1A;
    } else if (source_keyword(&p, "PWM1B")) {
        *mode = VM_PIN_MODE_PWM1B;
    } else if (source_keyword(&p, "PWM2A")) {
        *mode = VM_PIN_MODE_PWM2A;
    } else if (source_keyword(&p, "PWM2B")) {
        *mode = VM_PIN_MODE_PWM2B;
    } else if (source_keyword(&p, "PWM3A")) {
        *mode = VM_PIN_MODE_PWM3A;
    } else if (source_keyword(&p, "PWM3B")) {
        *mode = VM_PIN_MODE_PWM3B;
    } else if (source_keyword(&p, "PWM4A")) {
        *mode = VM_PIN_MODE_PWM4A;
    } else if (source_keyword(&p, "PWM4B")) {
        *mode = VM_PIN_MODE_PWM4B;
    } else if (source_keyword(&p, "PWM5A")) {
        *mode = VM_PIN_MODE_PWM5A;
    } else if (source_keyword(&p, "PWM5B")) {
        *mode = VM_PIN_MODE_PWM5B;
    } else if (source_keyword(&p, "PWM6A")) {
        *mode = VM_PIN_MODE_PWM6A;
    } else if (source_keyword(&p, "PWM6B")) {
        *mode = VM_PIN_MODE_PWM6B;
    } else if (source_keyword(&p, "PWM7A")) {
        *mode = VM_PIN_MODE_PWM7A;
    } else if (source_keyword(&p, "PWM7B")) {
        *mode = VM_PIN_MODE_PWM7B;
#ifdef rp2350
    } else if (source_keyword(&p, "PWM8A")) {
        *mode = VM_PIN_MODE_PWM8A;
    } else if (source_keyword(&p, "PWM8B")) {
        *mode = VM_PIN_MODE_PWM8B;
    } else if (source_keyword(&p, "PWM9A")) {
        *mode = VM_PIN_MODE_PWM9A;
    } else if (source_keyword(&p, "PWM9B")) {
        *mode = VM_PIN_MODE_PWM9B;
    } else if (source_keyword(&p, "PWM10A")) {
        *mode = VM_PIN_MODE_PWM10A;
    } else if (source_keyword(&p, "PWM10B")) {
        *mode = VM_PIN_MODE_PWM10B;
    } else if (source_keyword(&p, "PWM11A")) {
        *mode = VM_PIN_MODE_PWM11A;
    } else if (source_keyword(&p, "PWM11B")) {
        *mode = VM_PIN_MODE_PWM11B;
#endif
    } else if (source_keyword(&p, "PWM")) {
        *mode = -1;
    } else if (*p == '0' && !isnamechar((unsigned char)p[1])) {
        *mode = VM_PIN_MODE_OFF;
        p++;
    } else {
        return 0;
    }
    *pp = p;
    return 1;
}

static int source_try_emit_gp_pin(BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    source_skip_space(&p);
    if (strncasecmp(p, "GP", 2) != 0 || !isdigit((unsigned char)p[2]))
        return 0;

    const char *digits = p + 2;
    char *end = NULL;
    long gpio = strtol(digits, &end, 10);
    if (end == digits || isnamechar((unsigned char)*end))
        return 0;

    bc_emit_byte(cs, OP_PUSH_INT);
    bc_emit_i64(cs, -(int64_t)gpio - 1);
    *pp = end;
    return 1;
}

static void source_compile_pin_operand(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    if (source_try_emit_gp_pin(cs, pp))
        return;
    uint8_t type = source_parse_expression(fe, cs, pp);
    source_emit_int_conversion(cs, type);
}

static void source_emit_store_converted(BCCompiler *cs, uint16_t slot,
                                        uint8_t vtype, uint8_t etype,
                                        int is_local) {
    if ((vtype & T_INT) && (etype & T_NBR)) bc_emit_byte(cs, OP_CVT_F2I);
    else if ((vtype & T_NBR) && (etype & T_INT)) bc_emit_byte(cs, OP_CVT_I2F);
    bc_emit_store_var(cs, slot, vtype, is_local);
}

static void source_emit_int_conversion(BCCompiler *cs, uint8_t type) {
    if (type == T_NBR) bc_emit_byte(cs, OP_CVT_F2I);
    else if (type != T_INT) bc_set_error(cs, "Expected numeric expression");
}

static void source_emit_float_conversion(BCCompiler *cs, uint8_t type) {
    if (type == T_INT) bc_emit_byte(cs, OP_CVT_I2F);
    else if (type != T_NBR) bc_set_error(cs, "Expected numeric expression");
}

static int source_expect_char(BCCompiler *cs, const char **pp, char ch, const char *msg) {
    const char *p = *pp;
    source_skip_space(&p);
    if (*p != ch) {
        bc_set_error(cs, "%s", msg);
        *pp = p;
        return 0;
    }
    *pp = p + 1;
    return 1;
}

static int source_name_eq(const char *name, int name_len, const char *want) {
    int want_len = (int)strlen(want);
    return name_len == want_len && strncasecmp(name, want, want_len) == 0;
}

static uint8_t source_parse_as_type_clause(const char **pp) {
    const char *p = *pp;
    source_skip_space(&p);
    if (!source_keyword(&p, "AS")) return 0;
    source_skip_space(&p);

    uint8_t type = 0;
    if (strncasecmp(p, "INTEGER", 7) == 0 && !isnamechar((unsigned char)p[7])) {
        type = T_INT;
        p += 7;
    } else if (strncasecmp(p, "FLOAT", 5) == 0 && !isnamechar((unsigned char)p[5])) {
        type = T_NBR;
        p += 5;
    } else if (strncasecmp(p, "STRING", 6) == 0 && !isnamechar((unsigned char)p[6])) {
        type = T_STR;
        p += 6;
    } else {
        return 0;
    }

    *pp = p;
    return type;
}

static uint8_t source_compile_rgb_call(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    source_skip_space(&p);
    if (*p != '(') {
        bc_set_error(cs, "Expected '(' after RGB");
        *pp = p;
        return 0;
    }
    p++;

    const char *arg_start = p;
    int depth = 1;
    int comma_count = 0;
    const char *arg_end = NULL;
    const char *scan = p;
    while (*scan && depth > 0) {
        if (*scan == '"') {
            scan++;
            while (*scan && *scan != '"') scan++;
        } else if (*scan == '(') {
            depth++;
        } else if (*scan == ')') {
            depth--;
            if (depth == 0) {
                arg_end = scan;
                break;
            }
        } else if (*scan == ',' && depth == 1) {
            comma_count++;
        }
        if (*scan) scan++;
    }
    if (!arg_end) {
        bc_set_error(cs, "Expected ')' after RGB");
        *pp = p;
        return 0;
    }

    if (comma_count == 0) {
        int color = 0;
        if (!source_color_name(arg_start, arg_end, &color)) {
            bc_set_error(cs, "Unknown RGB colour name");
            *pp = p;
            return 0;
        }
        bc_emit_byte(cs, OP_PUSH_INT);
        bc_emit_i64(cs, color);
        *pp = arg_end + 1;
        return T_INT;
    }

    if (comma_count != 2) {
        bc_set_error(cs, "RGB requires one colour name or three components");
        *pp = p;
        return 0;
    }

    uint8_t type = source_parse_expression(fe, cs, &p);
    source_emit_int_conversion(cs, type);
    source_skip_space(&p);
    if (*p != ',') {
        bc_set_error(cs, "Expected ',' in RGB");
        *pp = p;
        return 0;
    }
    p++;

    type = source_parse_expression(fe, cs, &p);
    source_emit_int_conversion(cs, type);
    source_skip_space(&p);
    if (*p != ',') {
        bc_set_error(cs, "Expected ',' in RGB");
        *pp = p;
        return 0;
    }
    p++;

    type = source_parse_expression(fe, cs, &p);
    source_emit_int_conversion(cs, type);
    source_skip_space(&p);
    if (*p != ')') {
        bc_set_error(cs, "Expected ')' after RGB");
        *pp = p;
        return 0;
    }
    bc_emit_byte(cs, OP_SYSCALL);
    bc_emit_u16(cs, BC_SYS_GFX_RGB);
    bc_emit_byte(cs, 3);
    bc_emit_byte(cs, 0);
    *pp = p + 1;
    return T_INT;
}

static int source_parse_array_indices(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    source_skip_space(&p);
    if (*p != '(') {
        bc_set_error(cs, "Expected '(' for array access");
        *pp = p;
        return 0;
    }
    p++;
    source_skip_space(&p);
    if (*p == ')') {
        *pp = p + 1;
        return 0;
    }

    int ndim = 0;
    while (!cs->has_error) {
        uint8_t itype = source_parse_expression(fe, cs, &p);
        source_emit_int_conversion(cs, itype);
        ndim++;
        source_skip_space(&p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == ')') {
            p++;
            break;
        }
        bc_set_error(cs, "Expected ',' or ')' in array access");
        break;
    }

    *pp = p;
    return ndim;
}

static void source_emit_load_array(BCCompiler *cs, uint16_t slot, uint8_t type,
                                   int is_local, int ndim) {
    uint8_t op;
    if (is_local) {
        op = (type == T_INT) ? OP_LOAD_LOCAL_ARR_I :
             (type == T_STR) ? OP_LOAD_LOCAL_ARR_S :
                               OP_LOAD_LOCAL_ARR_F;
    } else {
        op = (type == T_INT) ? OP_LOAD_ARR_I :
             (type == T_STR) ? OP_LOAD_ARR_S :
                               OP_LOAD_ARR_F;
    }
    bc_emit_byte(cs, op);
    bc_emit_u16(cs, slot);
    bc_emit_byte(cs, (uint8_t)ndim);
}

static void source_emit_store_array(BCCompiler *cs, uint16_t slot, uint8_t type,
                                    int is_local, int ndim) {
    uint8_t op;
    if (is_local) {
        op = (type == T_INT) ? OP_STORE_LOCAL_ARR_I :
             (type == T_STR) ? OP_STORE_LOCAL_ARR_S :
                               OP_STORE_LOCAL_ARR_F;
    } else {
        op = (type == T_INT) ? OP_STORE_ARR_I :
             (type == T_STR) ? OP_STORE_ARR_S :
                               OP_STORE_ARR_F;
    }
    bc_emit_byte(cs, op);
    bc_emit_u16(cs, slot);
    bc_emit_byte(cs, (uint8_t)ndim);
}

static uint8_t source_parse_primary(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    source_skip_space(&p);

    if (*p == '"') {
        p++;
        const char *start = p;
        while (*p && *p != '"') p++;
        if (*p != '"') {
            bc_set_error(cs, "Unterminated string literal");
            *pp = p;
            return 0;
        }
        uint16_t idx = bc_add_constant_string(cs, (const uint8_t *)start, (int)(p - start));
        bc_emit_byte(cs, OP_PUSH_STR);
        bc_emit_u16(cs, idx);
        *pp = p + 1;
        return T_STR;
    }

    if (*p == '(') {
        p++;
        uint8_t type = source_parse_expression(fe, cs, &p);
        source_skip_space(&p);
        if (*p != ')') {
            bc_set_error(cs, "Expected ')'");
            *pp = p;
            return 0;
        }
        *pp = p + 1;
        return type;
    }

    {
        const char *q = p;
        if (source_keyword(&q, "MM.HRES")) {
            source_emit_syscall_noaux(cs, BC_SYS_MM_HRES, 0);
            *pp = q;
            return T_INT;
        }
        q = p;
        if (source_keyword(&q, "MM.VRES")) {
            source_emit_syscall_noaux(cs, BC_SYS_MM_VRES, 0);
            *pp = q;
            return T_INT;
        }
    }

    if (strncasecmp(p, "MM.INFO", 7) == 0) {
        const char *q = p + 7;
        source_skip_space(&q);
        if (*q != '(') {
            bc_set_error(cs, "Expected '(' after MM.INFO");
            *pp = q;
            return 0;
        }
        q++;
        source_skip_space(&q);
        if (source_keyword(&q, "HRES")) {
            source_emit_syscall_noaux(cs, BC_SYS_MM_HRES, 0);
        } else if (source_keyword(&q, "VRES")) {
            source_emit_syscall_noaux(cs, BC_SYS_MM_VRES, 0);
        } else {
            bc_set_error(cs, "Unsupported VM function: MM.INFO");
            *pp = q;
            return 0;
        }
        source_skip_space(&q);
        if (*q != ')') {
            bc_set_error(cs, "Expected ')' after MM.INFO");
            *pp = q;
            return 0;
        }
        *pp = q + 1;
        return T_INT;
    }

    if (*p == '&') {
        p++;
        int64_t ival = 0;
        char base = (char)toupper((unsigned char)*p++);
        if (base == 'H') {
            if (!isxdigit((unsigned char)*p)) bc_set_error(cs, "Invalid hexadecimal literal");
            while (isxdigit((unsigned char)*p)) {
                int d = toupper((unsigned char)*p) >= 'A' ? toupper((unsigned char)*p) - 'A' + 10
                                                          : *p - '0';
                ival = (ival << 4) | d;
                p++;
            }
        } else if (base == 'O') {
            if (*p < '0' || *p > '7') bc_set_error(cs, "Invalid octal literal");
            while (*p >= '0' && *p <= '7') {
                ival = (ival << 3) | (*p - '0');
                p++;
            }
        } else if (base == 'B') {
            if (*p != '0' && *p != '1') bc_set_error(cs, "Invalid binary literal");
            while (*p == '0' || *p == '1') {
                ival = (ival << 1) | (*p - '0');
                p++;
            }
        } else {
            bc_set_error(cs, "Invalid number base prefix");
        }
        bc_emit_byte(cs, OP_PUSH_INT);
        bc_emit_i64(cs, ival);
        *pp = p;
        return T_INT;
    }

    if (isdigit((unsigned char)*p) || *p == '.') {
        char *end = NULL;
        double v = strtod(p, &end);
        if (end == p) {
            bc_set_error(cs, "Invalid number");
            *pp = p;
            return 0;
        }

        int is_float = 0;
        for (const char *q = p; q < end; q++) {
            if (*q == '.' || *q == 'e' || *q == 'E') {
                is_float = 1;
                break;
            }
        }

        if (is_float) {
            bc_emit_byte(cs, OP_PUSH_FLT);
            bc_emit_f64(cs, (MMFLOAT)v);
            *pp = end;
            return T_NBR;
        }

        bc_emit_byte(cs, OP_PUSH_INT);
        bc_emit_i64(cs, (int64_t)strtoll(p, NULL, 10));
        *pp = end;
        return T_INT;
    }

    if (isnamestart((unsigned char)*p)) {
        char name[MAXVARLEN + 1];
        int name_len = 0;
        uint8_t type = 0;
        if (!source_parse_varname(&p, name, &name_len, &type)) {
            bc_set_error(cs, "Expected variable");
            *pp = p;
            return 0;
        }
        uint8_t suffix_type = type;
        if (type == 0) type = source_default_var_type(cs, name, name_len);

        const char *after_name = p;
        source_skip_space(&after_name);
        if (source_name_eq(name, name_len, "INKEY$")) {
            bc_emit_byte(cs, OP_STR_INKEY);
            *pp = p;
            return T_STR;
        }

        if (source_name_eq(name, name_len, "DATE$")) {
            source_emit_syscall_noaux(cs, BC_SYS_DATE_STR, 0);
            *pp = p;
            return T_STR;
        }

        if (source_name_eq(name, name_len, "TIME$")) {
            source_emit_syscall_noaux(cs, BC_SYS_TIME_STR, 0);
            *pp = p;
            return T_STR;
        }

        if (source_name_eq(name, name_len, "TIMER")) {
            bc_emit_byte(cs, OP_TIMER);
            *pp = p;
            return T_NBR;
        }

        if (source_name_eq(name, name_len, "PI")) {
            bc_emit_byte(cs, OP_MATH_PI);
            *pp = p;
            return T_NBR;
        }

        if (source_name_eq(name, name_len, "RND")) {
            bc_emit_byte(cs, OP_RND);
            *pp = p;
            return T_NBR;
        }

        if (source_name_eq(name, name_len, "LEN") && *after_name == '(') {
            p = after_name + 1;
            uint8_t arg_type = source_parse_expression(fe, cs, &p);
            if (arg_type != T_STR) bc_set_error(cs, "LEN requires a string argument");
            if (!source_expect_char(cs, &p, ')', "Expected ')' after LEN")) return 0;
            bc_emit_byte(cs, OP_STR_LEN);
            *pp = p;
            return T_INT;
        }

        if ((source_name_eq(name, name_len, "LEFT$") ||
             source_name_eq(name, name_len, "RIGHT$")) && *after_name == '(') {
            int is_left = source_name_eq(name, name_len, "LEFT$");
            p = after_name + 1;
            uint8_t str_type = source_parse_expression(fe, cs, &p);
            if (str_type != T_STR) bc_set_error(cs, "String function requires a string argument");
            if (!source_expect_char(cs, &p, ',', "Expected ',' in string function")) return 0;
            uint8_t count_type = source_parse_expression(fe, cs, &p);
            source_emit_int_conversion(cs, count_type);
            if (!source_expect_char(cs, &p, ')', "Expected ')' after string function")) return 0;
            bc_emit_byte(cs, is_left ? OP_STR_LEFT : OP_STR_RIGHT);
            *pp = p;
            return T_STR;
        }

        if (source_name_eq(name, name_len, "MID$") && *after_name == '(') {
            p = after_name + 1;
            uint8_t str_type = source_parse_expression(fe, cs, &p);
            if (str_type != T_STR) bc_set_error(cs, "MID$ requires a string argument");
            if (!source_expect_char(cs, &p, ',', "Expected ',' in MID$")) return 0;
            uint8_t start_type = source_parse_expression(fe, cs, &p);
            source_emit_int_conversion(cs, start_type);
            source_skip_space(&p);
            if (*p == ',') {
                p++;
                uint8_t len_type = source_parse_expression(fe, cs, &p);
                source_emit_int_conversion(cs, len_type);
                if (!source_expect_char(cs, &p, ')', "Expected ')' after MID$")) return 0;
                bc_emit_byte(cs, OP_STR_MID3);
            } else {
                if (!source_expect_char(cs, &p, ')', "Expected ')' after MID$")) return 0;
                bc_emit_byte(cs, OP_STR_MID2);
            }
            *pp = p;
            return T_STR;
        }

        if (source_name_eq(name, name_len, "VAL") && *after_name == '(') {
            p = after_name + 1;
            uint8_t arg_type = source_parse_expression(fe, cs, &p);
            if (arg_type != T_STR) bc_set_error(cs, "VAL requires a string argument");
            if (!source_expect_char(cs, &p, ')', "Expected ')' after VAL")) return 0;
            bc_emit_byte(cs, OP_STR_VAL);
            *pp = p;
            return T_NBR;
        }

        if (source_name_eq(name, name_len, "STR$") && *after_name == '(') {
            p = after_name + 1;
            uint8_t arg_type = source_parse_expression(fe, cs, &p);
            if (arg_type == T_STR) bc_set_error(cs, "STR$ requires a numeric argument");
            if (!source_expect_char(cs, &p, ')', "Expected ')' after STR$")) return 0;
            bc_emit_byte(cs, OP_STR_STR);
            *pp = p;
            return T_STR;
        }

        if (source_name_eq(name, name_len, "INSTR") && *after_name == '(') {
            p = after_name + 1;
            uint8_t first_type = source_parse_expression(fe, cs, &p);
            if (!source_expect_char(cs, &p, ',', "Expected ',' in INSTR")) return 0;
            if (first_type == T_STR) {
                uint8_t needle_type = source_parse_expression(fe, cs, &p);
                if (needle_type != T_STR) bc_set_error(cs, "INSTR requires string arguments");
                if (!source_expect_char(cs, &p, ')', "Expected ')' after INSTR")) return 0;
                bc_emit_byte(cs, OP_STR_INSTR);
                bc_emit_byte(cs, 2);
            } else {
                source_emit_int_conversion(cs, first_type);
                uint8_t haystack_type = source_parse_expression(fe, cs, &p);
                if (haystack_type != T_STR) bc_set_error(cs, "INSTR requires string arguments");
                if (!source_expect_char(cs, &p, ',', "Expected ',' in INSTR")) return 0;
                uint8_t needle_type = source_parse_expression(fe, cs, &p);
                if (needle_type != T_STR) bc_set_error(cs, "INSTR requires string arguments");
                if (!source_expect_char(cs, &p, ')', "Expected ')' after INSTR")) return 0;
                bc_emit_byte(cs, OP_STR_INSTR);
                bc_emit_byte(cs, 3);
            }
            *pp = p;
            return T_INT;
        }

        if ((source_name_eq(name, name_len, "HEX$") ||
             source_name_eq(name, name_len, "OCT$") ||
             source_name_eq(name, name_len, "BIN$")) && *after_name == '(') {
            uint8_t op = source_name_eq(name, name_len, "HEX$") ? OP_STR_HEX :
                         source_name_eq(name, name_len, "OCT$") ? OP_STR_OCT :
                                                                   OP_STR_BIN;
            p = after_name + 1;
            uint8_t arg_type = source_parse_expression(fe, cs, &p);
            source_emit_int_conversion(cs, arg_type);
            if (!source_expect_char(cs, &p, ')', "Expected ')' after numeric string function")) return 0;
            bc_emit_byte(cs, op);
            *pp = p;
            return T_STR;
        }

        if (source_name_eq(name, name_len, "SPACE$") && *after_name == '(') {
            p = after_name + 1;
            uint8_t arg_type = source_parse_expression(fe, cs, &p);
            source_emit_int_conversion(cs, arg_type);
            if (!source_expect_char(cs, &p, ')', "Expected ')' after SPACE$")) return 0;
            bc_emit_byte(cs, OP_STR_SPACE);
            *pp = p;
            return T_STR;
        }

        if (source_name_eq(name, name_len, "STRING$") && *after_name == '(') {
            p = after_name + 1;
            uint8_t count_type = source_parse_expression(fe, cs, &p);
            source_emit_int_conversion(cs, count_type);
            if (!source_expect_char(cs, &p, ',', "Expected ',' in STRING$")) return 0;
            uint8_t char_type = source_parse_expression(fe, cs, &p);
            if (char_type == T_STR) bc_emit_byte(cs, OP_STR_ASC);
            else source_emit_int_conversion(cs, char_type);
            if (!source_expect_char(cs, &p, ')', "Expected ')' after STRING$")) return 0;
            bc_emit_byte(cs, OP_STR_STRING);
            *pp = p;
            return T_STR;
        }

        if (source_name_eq(name, name_len, "FIELD$") && *after_name == '(') {
            p = after_name + 1;
            uint8_t src_type = source_parse_expression(fe, cs, &p);
            if (src_type != T_STR) bc_set_error(cs, "FIELD$ requires a string source");
            if (!source_expect_char(cs, &p, ',', "Expected ',' in FIELD$")) return 0;
            uint8_t field_type = source_parse_expression(fe, cs, &p);
            source_emit_int_conversion(cs, field_type);
            if (!source_expect_char(cs, &p, ',', "Expected ',' in FIELD$")) return 0;
            uint8_t delim_type = source_parse_expression(fe, cs, &p);
            if (delim_type != T_STR) bc_set_error(cs, "FIELD$ requires string delimiters");
            if (!source_expect_char(cs, &p, ')', "Expected ')' after FIELD$")) return 0;
            bc_emit_byte(cs, OP_STR_FIELD3);
            *pp = p;
            return T_STR;
        }

        if (source_name_eq(name, name_len, "RGB") && *after_name == '(') {
            p = after_name;
            uint8_t rgb_type = source_compile_rgb_call(fe, cs, &p);
            *pp = p;
            return rgb_type;
        }

        if (source_name_eq(name, name_len, "KEYDOWN") && *after_name == '(') {
            p = after_name + 1;
            uint8_t arg_type = source_parse_expression(fe, cs, &p);
            source_emit_int_conversion(cs, arg_type);
            source_skip_space(&p);
            if (*p != ')') bc_set_error(cs, "Expected ')' after KEYDOWN");
            else p++;
            source_emit_syscall_noaux(cs, BC_SYS_KEYDOWN, 1);
            *pp = p;
            return T_INT;
        }

        if (source_name_eq(name, name_len, "PIN") && *after_name == '(') {
            p = after_name + 1;
            source_compile_pin_operand(fe, cs, &p);
            if (!source_expect_char(cs, &p, ')', "Expected ')' after PIN")) return 0;
            source_emit_syscall_noaux(cs, BC_SYS_PIN_READ, 1);
            *pp = p;
            return T_INT;
        }

        if (source_name_eq(name, name_len, "PIXEL") && *after_name == '(') {
            p = after_name + 1;
            uint8_t x_type = source_parse_expression(fe, cs, &p);
            source_emit_int_conversion(cs, x_type);
            if (!source_expect_char(cs, &p, ',', "Expected ',' after PIXEL x")) return 0;
            uint8_t y_type = source_parse_expression(fe, cs, &p);
            source_emit_int_conversion(cs, y_type);
            if (!source_expect_char(cs, &p, ')', "Expected ')' after PIXEL")) return 0;
            source_emit_syscall_noaux(cs, BC_SYS_GFX_PIXEL_READ, 2);
            *pp = p;
            return T_INT;
        }

        if (source_name_eq(name, name_len, "MULSHR") && *after_name == '(') {
            uint32_t a_start, a_end, b_start, b_end;
            p = after_name + 1;
            a_start = cs->code_len;
            uint8_t a_type = source_parse_expression(fe, cs, &p);
            source_emit_int_conversion(cs, a_type);
            a_end = cs->code_len;
            if (!source_expect_char(cs, &p, ',', "Expected ',' after MULSHR a")) return 0;
            b_start = cs->code_len;
            uint8_t b_type = source_parse_expression(fe, cs, &p);
            source_emit_int_conversion(cs, b_type);
            b_end = cs->code_len;
            if (!source_expect_char(cs, &p, ',', "Expected ',' after MULSHR b")) return 0;
            uint8_t bits_type = source_parse_expression(fe, cs, &p);
            source_emit_int_conversion(cs, bits_type);
            if (!source_expect_char(cs, &p, ')', "Expected ')' after MULSHR")) return 0;
            if (bc_opt_level >= 1 &&
                source_is_same_simple_int_load(cs, a_start, a_end, b_start, b_end)) {
                source_delete_bytes(cs, b_start, b_end - b_start);
                if (cs->has_error) return 0;
                bc_emit_byte(cs, OP_MATH_SQRSHR);
            } else
                bc_emit_byte(cs, OP_MATH_MULSHR);
            *pp = p;
            return T_INT;
        }

        if (source_name_eq(name, name_len, "ASC") && *after_name == '(') {
            p = after_name + 1;
            uint8_t arg_type = source_parse_expression(fe, cs, &p);
            if (arg_type != T_STR) bc_set_error(cs, "ASC requires a string argument");
            source_skip_space(&p);
            if (*p != ')') bc_set_error(cs, "Expected ')' after ASC");
            else p++;
            bc_emit_byte(cs, OP_STR_ASC);
            *pp = p;
            return T_INT;
        }

        if (source_name_eq(name, name_len, "CHR$") && *after_name == '(') {
            p = after_name + 1;
            uint8_t arg_type = source_parse_expression(fe, cs, &p);
            source_emit_int_conversion(cs, arg_type);
            source_skip_space(&p);
            if (*p != ')') bc_set_error(cs, "Expected ')' after CHR$");
            else p++;
            bc_emit_byte(cs, OP_STR_CHR);
            *pp = p;
            return T_STR;
        }

        if ((source_name_eq(name, name_len, "LCASE$") ||
             source_name_eq(name, name_len, "UCASE$")) && *after_name == '(') {
            int is_lcase = source_name_eq(name, name_len, "LCASE$");
            p = after_name + 1;
            uint8_t arg_type = source_parse_expression(fe, cs, &p);
            if (arg_type != T_STR) bc_set_error(cs, "Case conversion requires a string argument");
            source_skip_space(&p);
            if (*p != ')') bc_set_error(cs, "Expected ')' after string function");
            else p++;
            bc_emit_byte(cs, is_lcase ? OP_STR_LCASE : OP_STR_UCASE);
            *pp = p;
            return T_STR;
        }

        if ((source_name_eq(name, name_len, "SIN") ||
             source_name_eq(name, name_len, "COS") ||
             source_name_eq(name, name_len, "TAN") ||
             source_name_eq(name, name_len, "ATN") ||
             source_name_eq(name, name_len, "ASIN") ||
             source_name_eq(name, name_len, "ACOS") ||
             source_name_eq(name, name_len, "SQR") ||
             source_name_eq(name, name_len, "LOG") ||
             source_name_eq(name, name_len, "EXP") ||
             source_name_eq(name, name_len, "RAD") ||
             source_name_eq(name, name_len, "DEG")) && *after_name == '(') {
            uint8_t op =
                source_name_eq(name, name_len, "SIN")  ? OP_MATH_SIN :
                source_name_eq(name, name_len, "COS")  ? OP_MATH_COS :
                source_name_eq(name, name_len, "TAN")  ? OP_MATH_TAN :
                source_name_eq(name, name_len, "ATN")  ? OP_MATH_ATN :
                source_name_eq(name, name_len, "ASIN") ? OP_MATH_ASIN :
                source_name_eq(name, name_len, "ACOS") ? OP_MATH_ACOS :
                source_name_eq(name, name_len, "SQR")  ? OP_MATH_SQR :
                source_name_eq(name, name_len, "LOG")  ? OP_MATH_LOG :
                source_name_eq(name, name_len, "EXP")  ? OP_MATH_EXP :
                source_name_eq(name, name_len, "RAD")  ? OP_MATH_RAD :
                                                        OP_MATH_DEG;
            p = after_name + 1;
            uint8_t arg_type = source_parse_expression(fe, cs, &p);
            source_emit_float_conversion(cs, arg_type);
            if (!source_expect_char(cs, &p, ')', "Expected ')' after math function")) return 0;
            bc_emit_byte(cs, op);
            *pp = p;
            return T_NBR;
        }

        if (source_name_eq(name, name_len, "ATAN2") && *after_name == '(') {
            p = after_name + 1;
            uint8_t y_type = source_parse_expression(fe, cs, &p);
            source_emit_float_conversion(cs, y_type);
            if (!source_expect_char(cs, &p, ',', "Expected ',' in ATAN2")) return 0;
            uint8_t x_type = source_parse_expression(fe, cs, &p);
            source_emit_float_conversion(cs, x_type);
            if (!source_expect_char(cs, &p, ')', "Expected ')' after ATAN2")) return 0;
            bc_emit_byte(cs, OP_MATH_ATAN2);
            *pp = p;
            return T_NBR;
        }

        if ((source_name_eq(name, name_len, "INT") ||
             source_name_eq(name, name_len, "ABS")) && *after_name == '(') {
            int is_int = source_name_eq(name, name_len, "INT");
            p = after_name + 1;
            uint8_t arg_type = source_parse_expression(fe, cs, &p);
            if (arg_type == T_STR) bc_set_error(cs, "Math function requires a numeric argument");
            if (is_int && arg_type == T_INT) bc_emit_byte(cs, OP_CVT_I2F);
            source_skip_space(&p);
            if (*p != ')') bc_set_error(cs, "Expected ')' after math function");
            else p++;
            bc_emit_byte(cs, is_int ? OP_MATH_INT : OP_MATH_ABS);
            *pp = p;
            return is_int ? T_NBR : arg_type;
        }

        if ((source_name_eq(name, name_len, "FIX") ||
             source_name_eq(name, name_len, "CINT")) && *after_name == '(') {
            int is_fix = source_name_eq(name, name_len, "FIX");
            p = after_name + 1;
            uint8_t arg_type = source_parse_expression(fe, cs, &p);
            source_emit_float_conversion(cs, arg_type);
            if (!source_expect_char(cs, &p, ')', "Expected ')' after math function")) return 0;
            bc_emit_byte(cs, is_fix ? OP_MATH_FIX : OP_MATH_CINT);
            *pp = p;
            return T_INT;
        }

        if (source_name_eq(name, name_len, "SGN") && *after_name == '(') {
            p = after_name + 1;
            uint8_t arg_type = source_parse_expression(fe, cs, &p);
            if (arg_type == T_STR) bc_set_error(cs, "SGN requires a numeric argument");
            if (!source_expect_char(cs, &p, ')', "Expected ')' after SGN")) return 0;
            bc_emit_byte(cs, OP_MATH_SGN);
            *pp = p;
            return T_INT;
        }

        if ((source_name_eq(name, name_len, "MIN") ||
             source_name_eq(name, name_len, "MAX")) && *after_name == '(') {
            int is_min = source_name_eq(name, name_len, "MIN");
            p = after_name + 1;
            uint8_t arg1_type = source_parse_expression(fe, cs, &p);
            source_emit_float_conversion(cs, arg1_type);
            if (!source_expect_char(cs, &p, ',', "Expected ',' in MIN/MAX")) return 0;
            uint8_t arg2_type = source_parse_expression(fe, cs, &p);
            source_emit_float_conversion(cs, arg2_type);
            if (!source_expect_char(cs, &p, ')', "Expected ')' after MIN/MAX")) return 0;
            bc_emit_byte(cs, is_min ? OP_MATH_MIN : OP_MATH_MAX);
            *pp = p;
            return T_NBR;
        }

        int sf_name_len = (suffix_type != 0 && name_len > 0) ? name_len - 1 : name_len;
        int sf_idx = bc_find_subfun(cs, name, sf_name_len);
        if (sf_idx >= 0 && cs->subfuns[sf_idx].return_type != 0 && *after_name == '(') {
            p = after_name;
            int nargs = source_compile_call_args(fe, cs, &p, 1);
            if (cs->has_error) {
                *pp = p;
                return 0;
            }
            bc_emit_byte(cs, OP_CALL_FUN);
            bc_emit_u16(cs, (uint16_t)sf_idx);
            bc_emit_byte(cs, (uint8_t)nargs);
            *pp = p;
            return cs->subfuns[sf_idx].return_type;
        }

        /* Check if this is a built-in interpreter function we should bridge.
         * Search tokentbl for T_FUN match (name with '(') and T_FNA match (name without). */
        {
            unsigned char tok_lookup[MAXVARLEN + 4];
            int tl_len = 0;
            int tok_idx = -1;
            uint8_t tok_flags = 0;

            /* Try T_FUN match: "NAME$(" */
            if (*after_name == '(') {
                memcpy(tok_lookup, name, name_len);
                tl_len = name_len;
                if (suffix_type == T_STR) tok_lookup[tl_len++] = '$';
                tok_lookup[tl_len++] = '(';
                tok_lookup[tl_len] = 0;
                for (int ti = 0; ti < TokenTableSize - 1; ti++) {
                    if (str_equal(tok_lookup, tokentbl[ti].name)) {
                        tok_idx = ti;
                        tok_flags = tokentbl[ti].type;
                        break;
                    }
                }
            }

            /* Try T_FNA match: "NAME$" (no-argument function) */
            if (tok_idx < 0) {
                memcpy(tok_lookup, name, name_len);
                tl_len = name_len;
                if (suffix_type == T_STR) tok_lookup[tl_len++] = '$';
                tok_lookup[tl_len] = 0;
                for (int ti = 0; ti < TokenTableSize - 1; ti++) {
                    if ((tokentbl[ti].type & T_FNA) &&
                        str_equal(tok_lookup, tokentbl[ti].name)) {
                        tok_idx = ti;
                        tok_flags = tokentbl[ti].type;
                        break;
                    }
                }
            }

            if (tok_idx >= 0) {
                /* Determine return type and bridge opcode */
                uint8_t ret_type;
                uint8_t bridge_op;
                if (tok_flags & T_STR) { ret_type = T_STR; bridge_op = OP_BRIDGE_FUN_S; }
                else if (tok_flags & T_INT) { ret_type = T_INT; bridge_op = OP_BRIDGE_FUN_I; }
                else { ret_type = T_NBR; bridge_op = OP_BRIDGE_FUN_F; }

                uint16_t tok_len = 0;
                unsigned char saved_inpbuf[STRINGSIZE];
                unsigned char saved_tknbuf[STRINGSIZE];

                if (tok_flags & T_FUN) {
                    /* Function with arguments: scan to matching ')' in source */
                    const char *paren = after_name; /* points to '(' */
                    int depth = 1;
                    const char *scan = paren + 1;
                    while (*scan && depth > 0) {
                        if (*scan == '(') depth++;
                        else if (*scan == ')') depth--;
                        scan++;
                    }
                    if (depth != 0) {
                        bc_set_error(cs, "Unmatched parenthesis in bridged function");
                        *pp = p;
                        return 0;
                    }

                    /* Tokenize the arguments (between parens, exclusive).
                     * Prepend "?" so tokenise() enters non-firstnonwhite mode,
                     * allowing nested function tokens to be recognized.
                     * The "?" tokenizes as a 2-byte PRINT command prefix we skip. */
                    char call_text[STRINGSIZE];
                    call_text[0] = '?';
                    size_t args_len = (size_t)(scan - 1 - (paren + 1)); /* exclude outer ( and ) */
                    if (args_len >= STRINGSIZE - 2) args_len = STRINGSIZE - 2;
                    memcpy(call_text + 1, paren + 1, args_len);
                    call_text[1 + args_len] = 0;

                    memcpy(saved_inpbuf, inpbuf, STRINGSIZE);
                    memcpy(saved_tknbuf, tknbuf, STRINGSIZE);
                    memcpy(inpbuf, call_text, args_len + 2);
                    tokenise(1);

                    /* tknbuf: PRINT_cmd(2 bytes) + tokenized_args + 0x00
                     * Skip the 2-byte PRINT command prefix. */
                    unsigned char *tp = tknbuf + 2;
                    unsigned char *tp_start = tp;
                    while (*tp) {
                        if (*tp == T_LINENBR) { tp += 3; continue; }
                        tp++;
                    }
                    tok_len = (uint16_t)(tp - tp_start);

                    bc_emit_byte(cs, bridge_op);
                    bc_emit_u16(cs, (uint16_t)tok_idx);
                    bc_emit_u16(cs, tok_len);
                    for (uint16_t i = 0; i < tok_len; i++)
                        bc_emit_byte(cs, tp_start[i]);

                    memcpy(inpbuf, saved_inpbuf, STRINGSIZE);
                    memcpy(tknbuf, saved_tknbuf, STRINGSIZE);
                    p = scan; /* advance past closing ')' */
                } else {
                    /* T_FNA: no arguments */
                    bc_emit_byte(cs, bridge_op);
                    bc_emit_u16(cs, (uint16_t)tok_idx);
                    bc_emit_u16(cs, 0);
                    /* p stays where it was — no args to consume */
                }

                *pp = p;
                return ret_type;
            }
        }

        if (*after_name == '(') {
            p = after_name;
            int ndim = source_parse_array_indices(fe, cs, &p);
            if (cs->has_error) {
                *pp = p;
                return 0;
            }
            int is_local = 0;
            uint16_t slot = source_resolve_var(cs, name, name_len, type, 1, &is_local);
            if (!is_local && slot != 0xFFFF) cs->slots[slot].is_array = 1;
            source_emit_load_array(cs, slot, type, is_local, ndim);
            *pp = p;
            return type;
        }

        int is_local = 0;
        uint16_t slot = source_resolve_var(cs, name, name_len, type, 1, &is_local);
        if (slot == 0xFFFF) {
            *pp = p;
            return 0;
        }
        /* Inline known constants — emit literal instead of slot load */
        if (!is_local && slot < cs->slot_count && cs->slots[slot].is_const) {
            if (cs->slots[slot].type & T_INT) {
                bc_emit_byte(cs, OP_PUSH_INT);
                bc_emit_i64(cs, cs->slots[slot].const_ival);
                *pp = p;
                return T_INT;
            } else if (cs->slots[slot].type & T_NBR) {
                bc_emit_byte(cs, OP_PUSH_FLT);
                bc_emit_f64(cs, cs->slots[slot].const_fval);
                *pp = p;
                return T_NBR;
            }
        }
        bc_emit_load_var(cs, slot, type, is_local);
        *pp = p;
        return type;
    }

    bc_set_error(cs, "Unsupported expression near: %.24s", p);
    *pp = p;
    return 0;
}

static uint8_t source_parse_unary(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    source_skip_space(&p);
    if (source_keyword(&p, "NOT")) {
        *pp = p;
        uint8_t type = source_parse_unary(fe, cs, pp);
        source_emit_int_conversion(cs, type);
        bc_emit_byte(cs, OP_NOT);
        return T_INT;
    }
    if (source_keyword(&p, "INV")) {
        *pp = p;
        uint8_t type = source_parse_unary(fe, cs, pp);
        source_emit_int_conversion(cs, type);
        bc_emit_byte(cs, OP_INV);
        return T_INT;
    }
    if (*p == '+') {
        p++;
        *pp = p;
        return source_parse_unary(fe, cs, pp);
    }
    if (*p == '-') {
        p++;
        *pp = p;
        uint8_t type = source_parse_unary(fe, cs, pp);
        if (cs->has_error) return 0;
        if (type == T_INT) bc_emit_byte(cs, OP_NEG_I);
        else if (type == T_NBR) bc_emit_byte(cs, OP_NEG_F);
        else bc_set_error(cs, "Unary '-' requires a numeric expression");
        return type;
    }
    return source_parse_primary(fe, cs, pp);
}

static uint8_t source_parse_power_expr(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    uint8_t left = source_parse_unary(fe, cs, pp);
    if (cs->has_error) return 0;

    while (1) {
        const char *p = *pp;
        source_skip_space(&p);
        if (*p != '^') break;
        p++;

        uint32_t right_start = cs->code_len;
        uint8_t right = source_parse_unary(fe, cs, &p);
        if (cs->has_error) return 0;
        if (left == T_INT) source_insert_byte(cs, right_start, OP_CVT_I2F);
        else if (left != T_NBR) bc_set_error(cs, "Power requires numeric expressions");
        if (right == T_INT) bc_emit_byte(cs, OP_CVT_I2F);
        else if (right != T_NBR) bc_set_error(cs, "Power requires numeric expressions");
        bc_emit_byte(cs, OP_POW_F);
        left = T_NBR;
        *pp = p;
    }

    return left;
}

static uint8_t source_emit_numeric_binary(BCCompiler *cs, uint8_t left, uint8_t right,
                                          uint32_t right_start, char op) {
    if ((left & T_STR) || (right & T_STR)) {
        if (op == '+' && (left & T_STR) && (right & T_STR)) {
            bc_emit_byte(cs, OP_ADD_S);
            return T_STR;
        }
        bc_set_error(cs, "Invalid string operator");
        return 0;
    }

    if (op == '/') {
        if (left == T_INT) source_insert_byte(cs, right_start, OP_CVT_I2F);
        if (right == T_INT) bc_emit_byte(cs, OP_CVT_I2F);
        bc_emit_byte(cs, OP_DIV_F);
        return T_NBR;
    }

    if (op == '\\') {
        if (left == T_NBR) source_insert_byte(cs, right_start, OP_CVT_F2I);
        if (right == T_NBR) bc_emit_byte(cs, OP_CVT_F2I);
        bc_emit_byte(cs, OP_IDIV_I);
        return T_INT;
    }

    if (op == 'm') {
        if (left == T_NBR) source_insert_byte(cs, right_start, OP_CVT_F2I);
        if (right == T_NBR) bc_emit_byte(cs, OP_CVT_F2I);
        bc_emit_byte(cs, OP_MOD_I);
        return T_INT;
    }

    if (left == T_NBR || right == T_NBR) {
        if (left == T_INT) source_insert_byte(cs, right_start, OP_CVT_I2F);
        if (right == T_INT) bc_emit_byte(cs, OP_CVT_I2F);
        switch (op) {
            case '+': bc_emit_byte(cs, OP_ADD_F); break;
            case '-': bc_emit_byte(cs, OP_SUB_F); break;
            case '*': bc_emit_byte(cs, OP_MUL_F); break;
            default:  bc_set_error(cs, "Unsupported numeric operator"); return 0;
        }
        return T_NBR;
    }

    switch (op) {
        case '+': bc_emit_byte(cs, OP_ADD_I); break;
        case '-': bc_emit_byte(cs, OP_SUB_I); break;
        case '*': bc_emit_byte(cs, OP_MUL_I); break;
        default:  bc_set_error(cs, "Unsupported numeric operator"); return 0;
    }
    return T_INT;
}

static int source_match_compare(const char **pp, char *op) {
    const char *p = *pp;
    source_skip_space(&p);
    if (p[0] == '<' && p[1] == '>') {
        *op = 'n';
        *pp = p + 2;
        return 1;
    }
    if (p[0] == '<' && p[1] == '=') {
        *op = 'l';
        *pp = p + 2;
        return 1;
    }
    if (p[0] == '>' && p[1] == '=') {
        *op = 'g';
        *pp = p + 2;
        return 1;
    }
    if (*p == '=') {
        *op = '=';
        *pp = p + 1;
        return 1;
    }
    if (*p == '<') {
        *op = '<';
        *pp = p + 1;
        return 1;
    }
    if (*p == '>') {
        *op = '>';
        *pp = p + 1;
        return 1;
    }
    return 0;
}

static uint8_t source_emit_compare(BCCompiler *cs, uint8_t left, uint8_t right,
                                   uint32_t right_start, char op) {
    if ((left & T_STR) || (right & T_STR)) {
        if (!(left & T_STR) || !(right & T_STR)) {
            bc_set_error(cs, "Cannot compare string with numeric expression");
            return 0;
        }
        switch (op) {
            case '=': bc_emit_byte(cs, OP_EQ_S); break;
            case 'n': bc_emit_byte(cs, OP_NE_S); break;
            case '<': bc_emit_byte(cs, OP_LT_S); break;
            case '>': bc_emit_byte(cs, OP_GT_S); break;
            case 'l': bc_emit_byte(cs, OP_LE_S); break;
            case 'g': bc_emit_byte(cs, OP_GE_S); break;
            default:  bc_set_error(cs, "Unsupported comparison"); return 0;
        }
        return T_INT;
    }

    if (left == T_NBR || right == T_NBR) {
        if (left == T_INT) source_insert_byte(cs, right_start, OP_CVT_I2F);
        if (right == T_INT) bc_emit_byte(cs, OP_CVT_I2F);
        switch (op) {
            case '=': bc_emit_byte(cs, OP_EQ_F); break;
            case 'n': bc_emit_byte(cs, OP_NE_F); break;
            case '<': bc_emit_byte(cs, OP_LT_F); break;
            case '>': bc_emit_byte(cs, OP_GT_F); break;
            case 'l': bc_emit_byte(cs, OP_LE_F); break;
            case 'g': bc_emit_byte(cs, OP_GE_F); break;
            default:  bc_set_error(cs, "Unsupported comparison"); return 0;
        }
        return T_INT;
    }

    switch (op) {
        case '=': bc_emit_byte(cs, OP_EQ_I); break;
        case 'n': bc_emit_byte(cs, OP_NE_I); break;
        case '<': bc_emit_byte(cs, OP_LT_I); break;
        case '>': bc_emit_byte(cs, OP_GT_I); break;
        case 'l': bc_emit_byte(cs, OP_LE_I); break;
        case 'g': bc_emit_byte(cs, OP_GE_I); break;
        default:  bc_set_error(cs, "Unsupported comparison"); return 0;
    }
    return T_INT;
}

static uint8_t source_emit_int_binary(BCCompiler *cs, uint8_t left, uint8_t right,
                                      uint32_t right_start, uint8_t op) {
    if (left == T_NBR) source_insert_byte(cs, right_start, OP_CVT_F2I);
    else if (left != T_INT) bc_set_error(cs, "Expected numeric expression");
    if (right == T_NBR) bc_emit_byte(cs, OP_CVT_F2I);
    else if (right != T_INT) bc_set_error(cs, "Expected numeric expression");
    bc_emit_byte(cs, op);
    return T_INT;
}

static uint8_t source_parse_mul_expr(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    uint32_t expr_start = cs->code_len;
    uint8_t left = source_parse_power_expr(fe, cs, pp);
    if (cs->has_error) return 0;

    while (1) {
        const char *p = *pp;
        source_skip_space(&p);
        char op = 0;
        if (*p == '*' || *p == '/' || *p == '\\') {
            op = *p++;
        } else {
            const char *q = p;
            if (source_keyword(&q, "MOD")) {
                op = 'm';
                p = q;
            } else {
                break;
            }
        }
        uint32_t right_start = cs->code_len;
        uint8_t right = source_parse_power_expr(fe, cs, &p);
        if (cs->has_error) return 0;
        if (op == '\\' && source_try_fuse_mulshr(cs, left, right, expr_start, right_start)) {
            left = T_INT;
            *pp = p;
            continue;
        }
        left = source_emit_numeric_binary(cs, left, right, right_start, op);
        *pp = p;
    }

    return left;
}

static uint8_t source_parse_add_expr(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    uint32_t expr_start = cs->code_len;
    uint8_t left = source_parse_mul_expr(fe, cs, pp);
    if (cs->has_error) return 0;

    while (1) {
        const char *p = *pp;
        source_skip_space(&p);
        if (*p != '+' && *p != '-') break;
        char op = *p++;
        uint32_t right_start = cs->code_len;
        uint8_t right = source_parse_mul_expr(fe, cs, &p);
        if (cs->has_error) return 0;
        if (source_try_fuse_mulshradd(cs, left, right, right_start, op)) {
            left = T_INT;
            *pp = p;
            continue;
        }
        left = source_emit_numeric_binary(cs, left, right, right_start, op);
        *pp = p;
    }

    return left;
}

static uint8_t source_parse_shift_expr(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    uint8_t left = source_parse_add_expr(fe, cs, pp);
    if (cs->has_error) return 0;

    while (1) {
        const char *p = *pp;
        source_skip_space(&p);
        uint8_t op = 0;
        if (p[0] == '<' && p[1] == '<') {
            op = OP_SHL;
            p += 2;
        } else if (p[0] == '>' && p[1] == '>') {
            op = OP_SHR;
            p += 2;
        } else {
            break;
        }
        uint32_t right_start = cs->code_len;
        uint8_t right = source_parse_add_expr(fe, cs, &p);
        if (cs->has_error) return 0;
        left = source_emit_int_binary(cs, left, right, right_start, op);
        *pp = p;
    }

    return left;
}

static uint8_t source_parse_expression(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    uint8_t left = source_parse_shift_expr(fe, cs, pp);
    if (cs->has_error) return 0;

    const char *p = *pp;
    char op = 0;
    if (source_match_compare(&p, &op)) {
        uint32_t right_start = cs->code_len;
        uint8_t right = source_parse_shift_expr(fe, cs, &p);
        if (cs->has_error) return 0;
        left = source_emit_compare(cs, left, right, right_start, op);
        *pp = p;
    }

    while (1) {
        p = *pp;
        source_skip_space(&p);
        uint8_t bool_op = 0;
        const char *q = p;
        if (source_keyword(&q, "AND")) {
            bool_op = OP_AND;
            p = q;
        } else if (source_keyword(&q, "OR")) {
            bool_op = OP_OR;
            p = q;
        } else if (source_keyword(&q, "XOR")) {
            bool_op = OP_XOR;
            p = q;
        } else {
            break;
        }

        uint32_t right_start = cs->code_len;
        uint8_t right = source_parse_shift_expr(fe, cs, &p);
        if (cs->has_error) return 0;

        char cmp_op = 0;
        if (source_match_compare(&p, &cmp_op)) {
            uint32_t cmp_right_start = cs->code_len;
            uint8_t cmp_right = source_parse_shift_expr(fe, cs, &p);
            if (cs->has_error) return 0;
            right = source_emit_compare(cs, right, cmp_right, cmp_right_start, cmp_op);
        }

        left = source_emit_int_binary(cs, left, right, right_start, bool_op);
        *pp = p;
    }

    return left;
}

static void source_compile_assignment(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    char name[MAXVARLEN + 1];
    int name_len = 0;
    uint8_t vtype = 0;
    if (!source_parse_varname(&p, name, &name_len, &vtype)) {
        bc_set_error(cs, "Expected variable");
        *pp = p;
        return;
    }

    if (source_name_eq(name, name_len, "PIN")) {
        uint8_t type;
        source_skip_space(&p);
        if (*p != '(') {
            bc_set_error(cs, "Expected '(' after PIN");
            *pp = p;
            return;
        }
        p++;
        source_compile_pin_operand(fe, cs, &p);
        if (cs->has_error) {
            *pp = p;
            return;
        }
        if (!source_expect_char(cs, &p, ')', "Expected ')' after PIN")) {
            *pp = p;
            return;
        }
        source_skip_space(&p);
        if (*p != '=') {
            bc_set_error(cs, "Expected '='");
            *pp = p;
            return;
        }
        p++;
        type = source_parse_expression(fe, cs, &p);
        source_emit_int_conversion(cs, type);
        bc_emit_byte(cs, OP_PIN_WRITE);
        *pp = p;
        return;
    }

    if (vtype == 0) vtype = source_default_var_type(cs, name, name_len);

    int is_local = 0;
    uint16_t slot = source_resolve_var(cs, name, name_len, vtype, 1, &is_local);
    source_skip_space(&p);
    if (*p == '(') {
        int ndim = source_parse_array_indices(fe, cs, &p);
        if (cs->has_error) {
            *pp = p;
            return;
        }
        if (!is_local && slot != 0xFFFF) cs->slots[slot].is_array = 1;
        source_skip_space(&p);
        if (*p != '=') {
            bc_set_error(cs, "Expected '='");
            *pp = p;
            return;
        }
        p++;

        uint8_t etype = source_parse_expression(fe, cs, &p);
        if (cs->has_error) {
            *pp = p;
            return;
        }
        if ((vtype & T_STR) && !(etype & T_STR)) {
            bc_set_error(cs, "Cannot assign numeric expression to string array");
            *pp = p;
            return;
        }
        if (!(vtype & T_STR) && (etype & T_STR)) {
            bc_set_error(cs, "Cannot assign string expression to numeric array");
            *pp = p;
            return;
        }
        if ((vtype & T_INT) && (etype & T_NBR)) bc_emit_byte(cs, OP_CVT_F2I);
        else if ((vtype & T_NBR) && (etype & T_INT)) bc_emit_byte(cs, OP_CVT_I2F);
        source_emit_store_array(cs, slot, vtype, is_local, ndim);
        *pp = p;
        return;
    }

    if (*p != '=') {
        bc_set_error(cs, "Expected '='");
        *pp = p;
        return;
    }
    p++;

    uint32_t expr_start = cs->code_len;
    uint8_t etype = source_parse_expression(fe, cs, &p);
    if (cs->has_error) {
        *pp = p;
        return;
    }

    if ((vtype & T_STR) && !(etype & T_STR)) {
        bc_set_error(cs, "Cannot assign numeric expression to string variable");
        *pp = p;
        return;
    }
    if (!(vtype & T_STR) && (etype & T_STR)) {
        bc_set_error(cs, "Cannot assign string expression to numeric variable");
        *pp = p;
        return;
    }
    if (source_try_fuse_mov_assignment(cs, expr_start, slot, is_local, vtype, etype)) {
        *pp = p;
        return;
    }
    source_emit_store_converted(cs, slot, vtype, etype, is_local);

    *pp = p;
}

static void source_compile_for(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    char name[MAXVARLEN + 1];
    int name_len = 0;
    uint8_t vtype = 0;
    if (!source_parse_varname(&p, name, &name_len, &vtype)) {
        bc_set_error(cs, "Expected variable after FOR");
        *pp = p;
        return;
    }
    if (vtype == 0) {
        vtype = source_default_var_type(cs, name, name_len);
    }

    int is_local = 0;
    source_skip_space(&p);
    if (*p != '=') {
        bc_set_error(cs, "Expected '=' in FOR");
        *pp = p;
        return;
    }
    p++;

    uint16_t var_slot = source_resolve_var(cs, name, name_len, vtype, 1, &is_local);
    uint8_t start_type = source_parse_expression(fe, cs, &p);
    if (cs->has_error) {
        *pp = p;
        return;
    }
    source_emit_store_converted(cs, var_slot, vtype, start_type, is_local);

    source_skip_space(&p);
    if (!source_keyword(&p, "TO")) {
        bc_set_error(cs, "Expected TO in FOR");
        *pp = p;
        return;
    }

    int lim_is_local = (cs->current_subfun >= 0);
    uint16_t lim_slot, step_slot;
    if (lim_is_local) {
        lim_slot = source_alloc_hidden_local(cs, vtype);
        step_slot = source_alloc_hidden_local(cs, vtype);
    } else {
        lim_slot = source_alloc_hidden_slot(cs, vtype);
        step_slot = source_alloc_hidden_slot(cs, vtype);
    }

    uint8_t limit_type = source_parse_expression(fe, cs, &p);
    if (cs->has_error) {
        *pp = p;
        return;
    }
    source_emit_store_converted(cs, lim_slot, vtype, limit_type, lim_is_local);

    source_skip_space(&p);
    if (source_keyword(&p, "STEP")) {
        uint8_t step_type = source_parse_expression(fe, cs, &p);
        if (cs->has_error) {
            *pp = p;
            return;
        }
        source_emit_store_converted(cs, step_slot, vtype, step_type, lim_is_local);
    } else {
        if (vtype == T_INT) bc_emit_byte(cs, OP_PUSH_ONE);
        else {
            bc_emit_byte(cs, OP_PUSH_FLT);
            bc_emit_f64(cs, 1.0);
        }
        bc_emit_store_var(cs, step_slot, vtype, lim_is_local);
    }

    uint16_t enc_var = var_slot | (is_local ? 0x8000 : 0);
    uint16_t enc_lim = lim_slot | (lim_is_local ? 0x8000 : 0);
    uint16_t enc_step = step_slot | (lim_is_local ? 0x8000 : 0);

    bc_emit_byte(cs, (vtype == T_INT) ? OP_FOR_INIT_I : OP_FOR_INIT_F);
    bc_emit_u16(cs, enc_var);
    bc_emit_u16(cs, enc_lim);
    bc_emit_u16(cs, enc_step);
    uint32_t exit_patch = cs->code_len;
    bc_emit_i16(cs, 0);

    uint32_t loop_top = cs->code_len;
    bc_nest_push(cs, NEST_FOR);
    BCNestEntry *ne = bc_nest_top(cs);
    if (ne) {
        ne->addr1 = loop_top;
        ne->addr2 = exit_patch;
        ne->var_slot = enc_var;
        ne->lim_slot = enc_lim;
        ne->step_slot = enc_step;
        ne->var_type = vtype;
    }

    *pp = p;
}

static void source_compile_next(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    (void)fe;
    BCNestEntry *ne = bc_nest_find(cs, NEST_FOR);
    if (!ne) {
        bc_set_error(cs, "NEXT without matching FOR");
        return;
    }

    bc_emit_byte(cs, (ne->var_type == T_INT) ? OP_FOR_NEXT_I : OP_FOR_NEXT_F);
    bc_emit_u16(cs, ne->var_slot);
    bc_emit_u16(cs, ne->lim_slot);
    bc_emit_u16(cs, ne->step_slot);
    bc_emit_i16(cs, (int16_t)(ne->addr1 - (cs->code_len + 2)));

    bc_patch_i16(cs, ne->addr2, (int16_t)(cs->code_len - (ne->addr2 + 2)));
    for (int i = 0; i < ne->exit_fixup_count; i++)
        source_patch_jmp_here(cs, ne->exit_fixups[i]);
    bc_nest_pop(cs);

    const char *p = *pp;
    source_skip_space(&p);
    if (isnamestart((unsigned char)*p)) {
        char name[MAXVARLEN + 1];
        int name_len = 0;
        uint8_t type = 0;
        source_parse_varname(&p, name, &name_len, &type);
    }
    *pp = p;
}

static int source_parse_file_number(BCCompiler *cs, const char **pp, int *fnbr) {
    const char *p = *pp;
    char *end = NULL;
    long value;

    source_skip_space(&p);
    if (*p != '#') {
        bc_set_error(cs, "Expected file number");
        *pp = p;
        return 0;
    }
    p++;
    source_skip_space(&p);
    if (!isdigit((unsigned char)*p)) {
        bc_set_error(cs, "Expected file number");
        *pp = p;
        return 0;
    }

    value = strtol(p, &end, 10);
    if (value < 1 || value > MAXOPENFILES) {
        bc_set_error(cs, "File number");
        *pp = end;
        return 0;
    }
    *fnbr = (int)value;
    *pp = end;
    return 1;
}

static void source_emit_file_print_expr(BCSourceFrontend *fe, BCCompiler *cs,
                                        const char **pp, int fnbr) {
    uint8_t type = source_parse_expression(fe, cs, pp);
    uint16_t sysid;
    uint8_t aux[2];

    if (cs->has_error) return;
    switch (type & (T_INT | T_NBR | T_STR)) {
        case T_INT: sysid = BC_SYS_FILE_PRINT_INT; break;
        case T_NBR: sysid = BC_SYS_FILE_PRINT_FLT; break;
        case T_STR: sysid = BC_SYS_FILE_PRINT_STR; break;
        default:
            bc_set_error(cs, "Invalid PRINT # expression");
            return;
    }
    aux[0] = (uint8_t)(fnbr & 0xFF);
    aux[1] = (uint8_t)(fnbr >> 8);
    source_emit_syscall(cs, sysid, 1, aux, 2);
}

static void source_compile_file_print(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    int fnbr = 0;
    int suppress_newline = 0;

    if (!source_parse_file_number(cs, &p, &fnbr)) {
        *pp = p;
        return;
    }
    source_skip_space(&p);
    if (*p == ',' || *p == ';') p++;

    while (*p && *p != '\'') {
        source_skip_space(&p);
        if (*p == '\0' || *p == '\'') break;
        if (*p == ';') {
            suppress_newline = 1;
            p++;
            continue;
        }
        if (*p == ',') {
            uint16_t tab = bc_add_constant_string(cs, (const uint8_t *)"\t", 1);
            bc_emit_byte(cs, OP_PUSH_STR);
            bc_emit_u16(cs, tab);
            {
                uint8_t aux[2] = {(uint8_t)(fnbr & 0xFF), (uint8_t)(fnbr >> 8)};
                source_emit_syscall(cs, BC_SYS_FILE_PRINT_STR, 1, aux, 2);
            }
            suppress_newline = 1;
            p++;
            continue;
        }

        suppress_newline = 0;
        source_emit_file_print_expr(fe, cs, &p, fnbr);
        if (cs->has_error) {
            *pp = p;
            return;
        }
        source_skip_space(&p);
        if (*p == ',' || *p == ';') continue;
        break;
    }

    if (!suppress_newline) {
        uint8_t aux[2] = {(uint8_t)(fnbr & 0xFF), (uint8_t)(fnbr >> 8)};
        source_emit_syscall(cs, BC_SYS_FILE_PRINT_NEWLINE, 0, aux, 2);
    }
    *pp = p;
}

static void source_compile_print(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    int suppress_newline = 0;

    source_skip_space(&p);
    if (*p == '#') {
        source_compile_file_print(fe, cs, &p);
        *pp = p;
        return;
    }

    while (*p && *p != '\'') {
        source_skip_space(&p);
        if (*p == '\0' || *p == '\'') break;
        if (*p == ';') {
            suppress_newline = 1;
            p++;
            continue;
        }
        if (*p == ',') {
            bc_emit_byte(cs, OP_PRINT_TAB);
            suppress_newline = 1;
            p++;
            continue;
        }

        suppress_newline = 0;
        uint8_t type = source_parse_expression(fe, cs, &p);
        if (cs->has_error) {
            *pp = p;
            return;
        }

        uint8_t op;
        switch (type & (T_INT | T_NBR | T_STR)) {
            case T_INT: op = OP_PRINT_INT; break;
            case T_NBR: op = OP_PRINT_FLT; break;
            case T_STR: op = OP_PRINT_STR; break;
            default:    op = OP_PRINT_INT; break;
        }
        bc_emit_byte(cs, op);
        bc_emit_byte(cs, PRINT_NO_NEWLINE);
        source_skip_space(&p);
        if (*p == ',' || *p == ';') continue;
        break;
    }

    if (!suppress_newline) bc_emit_byte(cs, OP_PRINT_NEWLINE);
    *pp = p;
}

static void source_compile_goto(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    (void)fe;
    const char *p = *pp;
    int lineno = source_parse_line_number(&p);
    if (lineno < 0) {
        bc_set_error(cs, "Expected line number after GOTO");
        *pp = p;
        return;
    }
    source_emit_abs_jump(cs, OP_JMP_ABS, lineno);
    *pp = p;
}

static void source_compile_gosub(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    (void)fe;
    const char *p = *pp;
    int lineno = source_parse_line_number(&p);
    if (lineno < 0) {
        bc_set_error(cs, "Expected line number after GOSUB");
        *pp = p;
        return;
    }
    source_emit_abs_jump(cs, OP_GOSUB, lineno);
    *pp = p;
}

static void source_compile_const(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;

    while (!cs->has_error) {
        char name[MAXVARLEN + 1];
        int name_len = 0;
        uint8_t vtype = 0;
        if (!source_parse_varname(&p, name, &name_len, &vtype)) {
            bc_set_error(cs, "Expected name in CONST");
            *pp = p;
            return;
        }

        source_skip_space(&p);
        if (*p != '=') {
            bc_set_error(cs, "Expected = in CONST");
            *pp = p;
            return;
        }
        p++;

        uint32_t expr_start = cs->code_len;
        uint8_t etype = source_parse_expression(fe, cs, &p);
        if (cs->has_error) {
            *pp = p;
            return;
        }
        if (vtype == 0) vtype = etype;

        if ((vtype & T_STR) && !(etype & T_STR)) {
            bc_set_error(cs, "Cannot assign numeric expression to string constant");
            *pp = p;
            return;
        }
        if (!(vtype & T_STR) && (etype & T_STR)) {
            bc_set_error(cs, "Cannot assign string expression to numeric constant");
            *pp = p;
            return;
        }
        uint16_t slot = source_resolve_global(cs, name, name_len, vtype, 1);

        /* Constant inlining: if the expression is a simple literal, record
         * the value in the slot and elide the store.  Every subsequent
         * LOAD of this slot will emit PUSH_INT/PUSH_FLT instead. */
        uint32_t expr_len = cs->code_len - expr_start;
        if (expr_len == 9 && cs->code[expr_start] == OP_PUSH_INT &&
            (vtype & T_INT)) {
            int64_t val;
            memcpy(&val, &cs->code[expr_start + 1], sizeof(val));
            cs->slots[slot].is_const = 1;
            cs->slots[slot].const_ival = val;
            cs->code_len = expr_start;  /* roll back — no runtime code needed */
        } else if (expr_len == 9 && cs->code[expr_start] == OP_PUSH_FLT &&
                   (vtype & T_NBR)) {
            double val;
            memcpy(&val, &cs->code[expr_start + 1], sizeof(val));
            cs->slots[slot].is_const = 1;
            cs->slots[slot].const_fval = val;
            cs->code_len = expr_start;
        } else {
            source_emit_store_converted(cs, slot, vtype, etype, 0);
        }

        source_skip_space(&p);
        if (*p != ',') break;
        p++;
    }

    *pp = p;
}

static void source_compile_dim(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    uint8_t forced_type = 0;

    source_skip_space(&p);
    if (strncasecmp(p, "INTEGER", 7) == 0 && !isnamechar((unsigned char)p[7])) {
        forced_type = T_INT;
        p += 7;
    } else if (strncasecmp(p, "FLOAT", 5) == 0 && !isnamechar((unsigned char)p[5])) {
        forced_type = T_NBR;
        p += 5;
    } else if (strncasecmp(p, "STRING", 6) == 0 && !isnamechar((unsigned char)p[6])) {
        forced_type = T_STR;
        p += 6;
    }

    while (!cs->has_error) {
        char name[MAXVARLEN + 1];
        int name_len = 0;
        uint8_t suffix_type = 0;
        if (!source_parse_varname(&p, name, &name_len, &suffix_type)) {
            bc_set_error(cs, "Expected name in DIM");
            *pp = p;
            return;
        }
        uint8_t vtype = suffix_type ? suffix_type : (forced_type ? forced_type : T_NBR);
        source_skip_space(&p);

        if (*p == '(') {
            p++;
            int ndim = 0;
            while (!cs->has_error) {
                uint8_t dtype = source_parse_expression(fe, cs, &p);
                source_emit_int_conversion(cs, dtype);
                ndim++;
                source_skip_space(&p);
                if (*p != ',') break;
                p++;
            }
            if (*p != ')') {
                bc_set_error(cs, "Expected ')' in DIM");
                *pp = p;
                return;
            }
            p++;
            uint8_t as_type = source_parse_as_type_clause(&p);
            if (as_type && !suffix_type) vtype = as_type;

            uint16_t slot = source_resolve_global(cs, name, name_len, vtype, 1);
            if (slot != 0xFFFF) cs->slots[slot].is_array = 1;
            uint8_t dim_op = (vtype == T_INT) ? OP_DIM_ARR_I :
                             (vtype == T_STR) ? OP_DIM_ARR_S :
                                                OP_DIM_ARR_F;
            bc_emit_byte(cs, dim_op);
            bc_emit_u16(cs, slot);
            bc_emit_byte(cs, (uint8_t)ndim);
        } else {
            uint8_t as_type = source_parse_as_type_clause(&p);
            if (as_type && !suffix_type) vtype = as_type;
            uint16_t slot = source_resolve_global(cs, name, name_len, vtype, 1);

            source_skip_space(&p);
            if (*p == '=') {
                p++;
                uint8_t etype = source_parse_expression(fe, cs, &p);
                if ((vtype & T_STR) && !(etype & T_STR)) {
                    bc_set_error(cs, "Cannot assign numeric expression to string variable");
                    *pp = p;
                    return;
                }
                if (!(vtype & T_STR) && (etype & T_STR)) {
                    bc_set_error(cs, "Cannot assign string expression to numeric variable");
                    *pp = p;
                    return;
                }
                source_emit_store_converted(cs, slot, vtype, etype, 0);
            }
        }

        source_skip_space(&p);
        if (*p != ',') break;
        p++;
    }

    *pp = p;
}

static void source_compile_data(BCCompiler *cs, const char **pp) {
    const char *p = *pp;

    while (!cs->has_error) {
        source_skip_space(&p);
        if (*p == '\0' || *p == '\'') break;
        if (cs->data_count >= BC_MAX_DATA_ITEMS) {
            bc_set_error(cs, "Too many DATA items");
            break;
        }

        BCDataItem *item = &cs->data_pool[cs->data_count];
        if (*p == '"') {
            p++;
            const char *start = p;
            while (*p && *p != '"') p++;
            uint16_t cidx = bc_add_constant_string(cs, (const uint8_t *)start, (int)(p - start));
            if (*p == '"') p++;
            item->value.i = cidx;
            item->type = T_STR;
            cs->data_count++;
        } else if (*p == '+' || *p == '-' || *p == '.' || isdigit((unsigned char)*p)) {
            char *end = NULL;
            double v = strtod(p, &end);
            if (end == p) {
                bc_set_error(cs, "Invalid DATA value");
                break;
            }
            int is_float = 0;
            for (const char *q = p; q < end; q++) {
                if (*q == '.' || *q == 'e' || *q == 'E') {
                    is_float = 1;
                    break;
                }
            }
            if (is_float) {
                item->value.f = (MMFLOAT)v;
                item->type = T_NBR;
            } else {
                item->value.i = (int64_t)strtoll(p, NULL, 10);
                item->type = T_INT;
            }
            p = end;
            cs->data_count++;
        } else {
            const char *start = p;
            while (*p && *p != ',' && *p != '\'') p++;
            const char *end = p;
            while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
            uint16_t cidx = bc_add_constant_string(cs, (const uint8_t *)start, (int)(end - start));
            item->value.i = cidx;
            item->type = T_STR;
            cs->data_count++;
        }

        source_skip_space(&p);
        if (*p != ',') break;
        p++;
    }

    while (*p && *p != '\'') p++;
    *pp = p;
}

static void source_compile_read(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;

    while (!cs->has_error) {
        char name[MAXVARLEN + 1];
        int name_len = 0;
        uint8_t vtype = 0;
        source_skip_space(&p);
        if (!source_parse_varname(&p, name, &name_len, &vtype)) {
            bc_set_error(cs, "Expected variable in READ");
            *pp = p;
            return;
        }
        if (vtype == 0) vtype = source_default_var_type(cs, name, name_len);

        const char *after_name = p;
        source_skip_space(&after_name);
        if (*after_name == '(') {
            p = after_name;
            int ndim = source_parse_array_indices(fe, cs, &p);
            int is_local = 0;
            uint16_t slot = source_resolve_var(cs, name, name_len, vtype, 1, &is_local);
            if (!is_local && slot != 0xFFFF) cs->slots[slot].is_array = 1;
            bc_emit_byte(cs, (vtype == T_INT) ? OP_READ_I :
                             (vtype == T_STR) ? OP_READ_S : OP_READ_F);
            source_emit_store_array(cs, slot, vtype, is_local, ndim);
        } else {
            p = after_name;
            int is_local = 0;
            uint16_t slot = source_resolve_var(cs, name, name_len, vtype, 1, &is_local);
            bc_emit_byte(cs, (vtype == T_INT) ? OP_READ_I :
                             (vtype == T_STR) ? OP_READ_S : OP_READ_F);
            bc_emit_store_var(cs, slot, vtype, is_local);
        }

        source_skip_space(&p);
        if (*p != ',') break;
        p++;
    }

    *pp = p;
}

static void source_compile_inc(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    char name[MAXVARLEN + 1];
    int name_len = 0;
    uint8_t vtype = 0;
    if (!source_parse_varname(&p, name, &name_len, &vtype)) {
        bc_set_error(cs, "Expected variable in INC");
        *pp = p;
        return;
    }
    if (vtype == 0) vtype = source_default_var_type(cs, name, name_len);
    if (vtype == T_STR) {
        bc_set_error(cs, "Unsupported source command: INC string");
        *pp = p;
        return;
    }

    int is_local = 0;
    uint16_t slot = source_resolve_var(cs, name, name_len, vtype, 1, &is_local);
    bc_emit_load_var(cs, slot, vtype, is_local);

    source_skip_space(&p);
    uint32_t right_start = cs->code_len;
    uint8_t amount_type;
    if (*p == ',') {
        p++;
        amount_type = source_parse_expression(fe, cs, &p);
    } else {
        bc_emit_byte(cs, OP_PUSH_ONE);
        amount_type = T_INT;
    }

    uint8_t result_type = source_emit_numeric_binary(cs, vtype, amount_type, right_start, '+');
    source_emit_store_converted(cs, slot, vtype, result_type, is_local);
    *pp = p;
}

static int source_parse_params(BCCompiler *cs, const char **pp, int sf_idx) {
    const char *p = *pp;
    int nparams = 0;
    source_skip_space(&p);
    if (*p != '(') {
        *pp = p;
        return 0;
    }
    p++;

    while (!cs->has_error) {
        source_skip_space(&p);
        if (*p == ')') {
            p++;
            break;
        }
        char name[MAXVARLEN + 1];
        int name_len = 0;
        uint8_t ptype = 0;
        if (!source_parse_varname(&p, name, &name_len, &ptype)) {
            bc_set_error(cs, "Expected parameter name");
            *pp = p;
            return nparams;
        }

        int is_array = 0;
        source_skip_space(&p);
        if (*p == '(') {
            const char *q = p + 1;
            source_skip_space(&q);
            if (*q == ')') {
                is_array = 1;
                p = q + 1;
            }
        }

        uint8_t as_type = source_parse_as_type_clause(&p);
        if (as_type != 0) ptype = as_type;
        if (ptype == 0) ptype = T_NBR;

        bc_add_local(cs, name, name_len, ptype, is_array);
        if (nparams < BC_MAX_PARAMS) {
            cs->subfuns[sf_idx].param_types[nparams] = ptype;
            cs->subfuns[sf_idx].param_is_array[nparams] = (uint8_t)is_array;
        }
        nparams++;

        source_skip_space(&p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == ')') {
            p++;
            break;
        }
        bc_set_error(cs, "Expected ',' or ')' in parameter list");
        break;
    }

    *pp = p;
    return nparams;
}

static int source_compile_call_args(BCSourceFrontend *fe, BCCompiler *cs, const char **pp,
                                    int require_parens) {
    const char *p = *pp;
    int nargs = 0;
    source_skip_space(&p);

    int has_parens = (*p == '(');
    if (has_parens) p++;
    else if (require_parens) {
        bc_set_error(cs, "Expected '(' in function call");
        *pp = p;
        return 0;
    } else if (*p == '\0' || *p == '\'') {
        *pp = p;
        return 0;
    }

    while (!cs->has_error) {
        source_skip_space(&p);
        if (has_parens && *p == ')') {
            p++;
            break;
        }
        if (!has_parens && (*p == '\0' || *p == '\'')) break;

        (void)source_parse_expression(fe, cs, &p);
        if (cs->has_error) break;
        nargs++;

        source_skip_space(&p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (has_parens) {
            if (*p == ')') {
                p++;
                break;
            }
            bc_set_error(cs, "Expected ',' or ')' in argument list");
        }
        break;
    }

    *pp = p;
    return nargs;
}

static void source_compile_local(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    (void)fe;
    const char *p = *pp;
    if (cs->current_subfun < 0) {
        bc_set_error(cs, "LOCAL outside SUB/FUNCTION");
        *pp = p;
        return;
    }

    uint8_t forced_type = 0;
    source_skip_space(&p);
    if (strncasecmp(p, "INTEGER", 7) == 0 && !isnamechar((unsigned char)p[7])) {
        forced_type = T_INT;
        p += 7;
    } else if (strncasecmp(p, "FLOAT", 5) == 0 && !isnamechar((unsigned char)p[5])) {
        forced_type = T_NBR;
        p += 5;
    } else if (strncasecmp(p, "STRING", 6) == 0 && !isnamechar((unsigned char)p[6])) {
        forced_type = T_STR;
        p += 6;
    }

    while (!cs->has_error) {
        char name[MAXVARLEN + 1];
        int name_len = 0;
        uint8_t vtype = 0;
        if (!source_parse_varname(&p, name, &name_len, &vtype)) {
            bc_set_error(cs, "Expected name in LOCAL");
            *pp = p;
            return;
        }

        int is_array = 0;
        source_skip_space(&p);
        if (*p == '(') {
            const char *q = p + 1;
            source_skip_space(&q);
            if (*q == ')') {
                is_array = 1;
                p = q + 1;
            }
        }

        uint8_t as_type = source_parse_as_type_clause(&p);
        if (as_type != 0) vtype = as_type;
        if (vtype == 0) vtype = forced_type ? forced_type : T_NBR;
        bc_add_local(cs, name, name_len, vtype, is_array);

        source_skip_space(&p);
        if (*p != ',') break;
        p++;
    }

    *pp = p;
}

static void source_compile_sub(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    (void)fe;
    const char *p = *pp;
    source_skip_space(&p);

    const char *name_start = p;
    char name[MAXVARLEN + 1];
    int name_len = 0;
    uint8_t unused_type = 0;
    if (!source_parse_varname(&p, name, &name_len, &unused_type)) {
        bc_set_error(cs, "Expected SUB name");
        *pp = p;
        return;
    }
    if (unused_type != 0) {
        bc_set_error(cs, "SUB name cannot have a type suffix");
        *pp = p;
        return;
    }

    int sf_idx = source_get_or_create_subfun(cs, name_start, name_len, 0);
    if (sf_idx < 0) {
        *pp = p;
        return;
    }

    uint32_t skip_patch = source_emit_jmp_placeholder(cs, OP_JMP);
    cs->subfuns[sf_idx].entry_addr = cs->code_len;
    cs->current_subfun = sf_idx;
    cs->local_count = 0;

    cs->subfuns[sf_idx].nparams = (uint8_t)source_parse_params(cs, &p, sf_idx);

    bc_emit_byte(cs, OP_ENTER_FRAME);
    uint32_t nlocals_patch = cs->code_len;
    bc_emit_u16(cs, 0);

    bc_nest_push(cs, NEST_SUB);
    BCNestEntry *ne = bc_nest_top(cs);
    if (ne) {
        ne->addr1 = skip_patch;
        ne->addr2 = nlocals_patch;
    }
    *pp = p;
}

static void source_compile_end_sub(BCCompiler *cs) {
    BCNestEntry *ne = bc_nest_top(cs);
    if (!ne || ne->type != NEST_SUB) {
        bc_set_error(cs, "END SUB without matching SUB");
        return;
    }
    bc_patch_u16(cs, ne->addr2, cs->local_count);
    if (cs->current_subfun >= 0) {
        cs->subfuns[cs->current_subfun].nlocals = cs->local_count;
        bc_commit_locals(cs, cs->current_subfun);
    }
    bc_emit_byte(cs, OP_LEAVE_FRAME);
    bc_emit_byte(cs, OP_RET_SUB);
    source_patch_jmp_here(cs, ne->addr1);
    cs->current_subfun = -1;
    cs->local_count = 0;
    bc_nest_pop(cs);
}

static void source_compile_function(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    (void)fe;
    const char *p = *pp;
    source_skip_space(&p);

    const char *name_start = p;
    char name[MAXVARLEN + 1];
    int name_len = 0;
    uint8_t ret_type = 0;
    if (!source_parse_varname(&p, name, &name_len, &ret_type)) {
        bc_set_error(cs, "Expected FUNCTION name");
        *pp = p;
        return;
    }
    int has_suffix = (ret_type != 0);
    int sf_name_len = has_suffix ? name_len - 1 : name_len;
    if (ret_type == 0) ret_type = T_NBR;

    int sf_idx = source_get_or_create_subfun(cs, name_start, sf_name_len, ret_type);
    if (sf_idx < 0) {
        *pp = p;
        return;
    }

    uint32_t skip_patch = source_emit_jmp_placeholder(cs, OP_JMP);
    cs->subfuns[sf_idx].entry_addr = cs->code_len;
    cs->current_subfun = sf_idx;
    cs->local_count = 0;

    bc_add_local(cs, name_start, has_suffix ? name_len : sf_name_len, ret_type, 0);
    cs->subfuns[sf_idx].nparams = (uint8_t)source_parse_params(cs, &p, sf_idx);
    uint8_t as_type = source_parse_as_type_clause(&p);
    if (as_type != 0 && !has_suffix) {
        ret_type = as_type;
        cs->subfuns[sf_idx].return_type = ret_type;
        cs->locals[0].type = ret_type;
    }

    bc_emit_byte(cs, OP_ENTER_FRAME);
    uint32_t nlocals_patch = cs->code_len;
    bc_emit_u16(cs, 0);

    bc_nest_push(cs, NEST_FUNCTION);
    BCNestEntry *ne = bc_nest_top(cs);
    if (ne) {
        ne->addr1 = skip_patch;
        ne->addr2 = nlocals_patch;
        ne->var_slot = 0;
        ne->var_type = ret_type;
    }
    *pp = p;
}

static void source_compile_end_function(BCCompiler *cs) {
    BCNestEntry *ne = bc_nest_top(cs);
    if (!ne || ne->type != NEST_FUNCTION) {
        bc_set_error(cs, "END FUNCTION without matching FUNCTION");
        return;
    }
    bc_patch_u16(cs, ne->addr2, cs->local_count);
    if (cs->current_subfun >= 0) {
        cs->subfuns[cs->current_subfun].nlocals = cs->local_count;
        bc_commit_locals(cs, cs->current_subfun);
    }
    bc_emit_load_var(cs, ne->var_slot, ne->var_type, 1);
    bc_emit_byte(cs, OP_LEAVE_FRAME);
    bc_emit_byte(cs, OP_RET_FUN);
    source_patch_jmp_here(cs, ne->addr1);
    cs->current_subfun = -1;
    cs->local_count = 0;
    bc_nest_pop(cs);
}

static void source_compile_do(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    source_skip_space(&p);

    uint32_t loop_top = cs->code_len;
    bc_nest_push(cs, NEST_DO);
    BCNestEntry *ne = bc_nest_top(cs);
    if (ne) {
        ne->addr1 = loop_top;
        ne->addr2 = 0xFFFFFFFF;
    }

    if (source_keyword(&p, "WHILE")) {
        uint8_t type = source_parse_expression(fe, cs, &p);
        if (type == T_STR) bc_set_error(cs, "DO WHILE requires a numeric condition");
        if (ne) ne->addr2 = source_emit_jmp_placeholder(cs, OP_JZ);
    } else if (source_keyword(&p, "UNTIL")) {
        uint8_t type = source_parse_expression(fe, cs, &p);
        if (type == T_STR) bc_set_error(cs, "DO UNTIL requires a numeric condition");
        if (ne) ne->addr2 = source_emit_jmp_placeholder(cs, OP_JNZ);
    }

    *pp = p;
}

static void source_compile_loop(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    BCNestEntry *ne = bc_nest_find(cs, NEST_DO);
    if (!ne) ne = bc_nest_find(cs, NEST_WHILE);
    if (!ne) {
        bc_set_error(cs, "LOOP without matching DO or WHILE");
        return;
    }

    const char *p = *pp;
    source_skip_space(&p);

    if (ne->type == NEST_WHILE) {
        bc_emit_byte(cs, OP_JMP);
        bc_emit_i16(cs, (int16_t)(ne->addr1 - (cs->code_len + 2)));
        source_patch_jmp_here(cs, ne->addr2);
        for (int i = 0; i < ne->exit_fixup_count; i++)
            source_patch_jmp_here(cs, ne->exit_fixups[i]);
        bc_nest_pop(cs);
        *pp = p;
        return;
    }

    if (source_keyword(&p, "WHILE")) {
        uint8_t type = source_parse_expression(fe, cs, &p);
        if (type == T_STR) bc_set_error(cs, "LOOP WHILE requires a numeric condition");
        source_emit_rel_jump(cs, OP_JNZ, ne->addr1);
    } else if (source_keyword(&p, "UNTIL")) {
        uint8_t type = source_parse_expression(fe, cs, &p);
        if (type == T_STR) bc_set_error(cs, "LOOP UNTIL requires a numeric condition");
        source_emit_rel_jump(cs, OP_JZ, ne->addr1);
    } else {
        bc_emit_byte(cs, OP_JMP);
        bc_emit_i16(cs, (int16_t)(ne->addr1 - (cs->code_len + 2)));
    }

    if (ne->addr2 != 0xFFFFFFFF) source_patch_jmp_here(cs, ne->addr2);
    for (int i = 0; i < ne->exit_fixup_count; i++)
        source_patch_jmp_here(cs, ne->exit_fixups[i]);
    bc_nest_pop(cs);
    *pp = p;
}

static void source_compile_exit(BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    source_skip_space(&p);

    if (source_keyword(&p, "DO")) {
        BCNestEntry *ne = bc_nest_find(cs, NEST_DO);
        if (!ne) {
            bc_set_error(cs, "EXIT DO without matching DO");
            *pp = p;
            return;
        }
        uint32_t patch = source_emit_jmp_placeholder(cs, OP_JMP);
        if (ne->exit_fixup_count < 64) ne->exit_fixups[ne->exit_fixup_count++] = patch;
        *pp = p;
        return;
    }

    if (source_keyword(&p, "FOR")) {
        BCNestEntry *ne = bc_nest_find(cs, NEST_FOR);
        if (!ne) {
            bc_set_error(cs, "EXIT FOR without matching FOR");
            *pp = p;
            return;
        }
        uint32_t patch = source_emit_jmp_placeholder(cs, OP_JMP);
        if (ne->exit_fixup_count < 64) ne->exit_fixups[ne->exit_fixup_count++] = patch;
        *pp = p;
        return;
    }

    if (source_keyword(&p, "FUNCTION")) {
        BCNestEntry *ne = bc_nest_find(cs, NEST_FUNCTION);
        if (!ne) {
            bc_set_error(cs, "EXIT FUNCTION without matching FUNCTION");
            *pp = p;
            return;
        }
        bc_emit_load_var(cs, ne->var_slot, ne->var_type, 1);
        bc_emit_byte(cs, OP_LEAVE_FRAME);
        bc_emit_byte(cs, OP_RET_FUN);
        *pp = p;
        return;
    }

    if (source_keyword(&p, "SUB")) {
        BCNestEntry *ne = bc_nest_find(cs, NEST_SUB);
        if (!ne) {
            bc_set_error(cs, "EXIT SUB without matching SUB");
            *pp = p;
            return;
        }
        bc_emit_byte(cs, OP_LEAVE_FRAME);
        bc_emit_byte(cs, OP_RET_SUB);
        *pp = p;
        return;
    }

    bc_set_error(cs, "Expected DO, FOR, FUNCTION or SUB after EXIT");
    *pp = p;
}

static void source_compile_fastgfx(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    source_skip_space(&p);

    if (source_keyword(&p, "CREATE")) {
        source_emit_syscall_noaux(cs, BC_SYS_FASTGFX_CREATE, 0);
        *pp = p;
        return;
    }
    if (source_keyword(&p, "CLOSE")) {
        source_emit_syscall_noaux(cs, BC_SYS_FASTGFX_CLOSE, 0);
        *pp = p;
        return;
    }
    if (source_keyword(&p, "SWAP")) {
        source_emit_syscall_noaux(cs, BC_SYS_FASTGFX_SWAP, 0);
        *pp = p;
        return;
    }
    if (source_keyword(&p, "SYNC")) {
        source_emit_syscall_noaux(cs, BC_SYS_FASTGFX_SYNC, 0);
        *pp = p;
        return;
    }
    if (source_keyword(&p, "FPS")) {
        uint8_t type = source_parse_expression(fe, cs, &p);
        source_emit_int_conversion(cs, type);
        source_emit_syscall_noaux(cs, BC_SYS_FASTGFX_FPS, 1);
        *pp = p;
        return;
    }

    bc_set_error(cs, "Unsupported FASTGFX command");
    *pp = p;
}

static int source_parse_framebuffer_target(const char **pp, char *target_out) {
    const char *p = *pp;
    source_skip_space(&p);
    if (*p == '"') {
        p++;
        if ((*p == 'N' || *p == 'n' || *p == 'F' || *p == 'f' || *p == 'L' || *p == 'l') &&
            p[1] == '"') {
            *target_out = (char)toupper((unsigned char)*p);
            p += 2;
            *pp = p;
            return 1;
        }
        return 0;
    }
    if (*p == 'N' || *p == 'n' || *p == 'F' || *p == 'f' || *p == 'L' || *p == 'l') {
        *target_out = (char)toupper((unsigned char)*p);
        p++;
        *pp = p;
        return 1;
    }
    return 0;
}

static int source_parse_framebuffer_merge_mode(const char **pp, uint8_t *mode_out) {
    const char *p = *pp;
    source_skip_space(&p);
    if (*p == '"') {
        p++;
        if ((*p == 'A' || *p == 'a' || *p == 'B' || *p == 'b' || *p == 'R' || *p == 'r') &&
            p[1] == '"') {
            char mode = (char)toupper((unsigned char)*p);
            *mode_out = (mode == 'A') ? BC_FB_MERGE_MODE_A :
                        (mode == 'B') ? BC_FB_MERGE_MODE_B : BC_FB_MERGE_MODE_R;
            p += 2;
            *pp = p;
            return 1;
        }
        return 0;
    }
    if (*p == 'A' || *p == 'a' || *p == 'B' || *p == 'b' || *p == 'R' || *p == 'r') {
        char mode = (char)toupper((unsigned char)*p);
        *mode_out = (mode == 'A') ? BC_FB_MERGE_MODE_A :
                    (mode == 'B') ? BC_FB_MERGE_MODE_B : BC_FB_MERGE_MODE_R;
        p++;
        *pp = p;
        return 1;
    }
    return 0;
}

static void source_compile_framebuffer(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    uint8_t aux[4];
    source_skip_space(&p);

    if (source_keyword(&p, "CREATE")) {
        source_emit_syscall(cs, BC_SYS_GFX_FRAMEBUFFER, 0, (uint8_t[]){BC_FB_OP_CREATE}, 1);
        *pp = p;
        return;
    }
    if (source_keyword(&p, "LAYER")) {
        source_skip_space(&p);
        if (source_keyword(&p, "TOP")) {
            bc_set_error(cs, "Unsupported FRAMEBUFFER LAYER TOP");
            *pp = p;
            return;
        }
        if (*p && *p != '\'') {
            uint8_t type = source_parse_expression(fe, cs, &p);
            source_emit_int_conversion(cs, type);
            aux[0] = BC_FB_OP_LAYER;
            aux[1] = 1;
            source_emit_syscall(cs, BC_SYS_GFX_FRAMEBUFFER, 1, aux, 2);
        } else {
            aux[0] = BC_FB_OP_LAYER;
            aux[1] = 0;
            source_emit_syscall(cs, BC_SYS_GFX_FRAMEBUFFER, 0, aux, 2);
        }
        *pp = p;
        return;
    }
    if (source_keyword(&p, "WRITE")) {
        char target = 0;
        if (!source_parse_framebuffer_target(&p, &target)) {
            bc_set_error(cs, "Unsupported FRAMEBUFFER WRITE target");
            *pp = p;
            return;
        }
        aux[0] = BC_FB_OP_WRITE;
        aux[1] = (uint8_t)target;
        source_emit_syscall(cs, BC_SYS_GFX_FRAMEBUFFER, 0, aux, 2);
        *pp = p;
        return;
    }
    if (source_keyword(&p, "CLOSE")) {
        char target = BC_FB_TARGET_DEFAULT;
        source_parse_framebuffer_target(&p, &target);
        aux[0] = BC_FB_OP_CLOSE;
        aux[1] = (uint8_t)target;
        source_emit_syscall(cs, BC_SYS_GFX_FRAMEBUFFER, 0, aux, 2);
        *pp = p;
        return;
    }
    if (source_keyword(&p, "MERGE")) {
        int argc = 0;
        uint8_t mode = BC_FB_MERGE_MODE_NOW;
        int has_colour = 0;
        int has_rate = 0;

        source_skip_space(&p);
        if (*p && *p != '\'') {
            uint8_t type = source_parse_expression(fe, cs, &p);
            source_emit_int_conversion(cs, type);
            argc++;
            has_colour = 1;
            source_skip_space(&p);
            if (*p == ',') {
                p++;
                if (!source_parse_framebuffer_merge_mode(&p, &mode)) {
                    bc_set_error(cs, "Unsupported FRAMEBUFFER MERGE mode");
                    *pp = p;
                    return;
                }
                source_skip_space(&p);
                if (*p == ',') {
                    p++;
                    {
                        uint8_t type2 = source_parse_expression(fe, cs, &p);
                        source_emit_int_conversion(cs, type2);
                        argc++;
                        has_rate = 1;
                    }
                }
            }
        }

        aux[0] = BC_FB_OP_MERGE;
        aux[1] = mode;
        aux[2] = (uint8_t)has_colour;
        aux[3] = (uint8_t)has_rate;
        source_emit_syscall(cs, BC_SYS_GFX_FRAMEBUFFER, (uint8_t)argc, aux, 4);
        *pp = p;
        return;
    }
    if (source_keyword(&p, "SYNC")) {
        source_emit_syscall(cs, BC_SYS_GFX_FRAMEBUFFER, 0, (uint8_t[]){BC_FB_OP_SYNC}, 1);
        *pp = p;
        return;
    }
    if (source_keyword(&p, "WAIT")) {
        source_emit_syscall(cs, BC_SYS_GFX_FRAMEBUFFER, 0, (uint8_t[]){BC_FB_OP_WAIT}, 1);
        *pp = p;
        return;
    }
    if (source_keyword(&p, "COPY")) {
        char from = 0;
        char to = 0;
        uint8_t background = 0;

        if (!source_parse_framebuffer_target(&p, &from)) {
            bc_set_error(cs, "Unsupported FRAMEBUFFER COPY source");
            *pp = p;
            return;
        }
        source_skip_space(&p);
        if (*p != ',') {
            bc_set_error(cs, "Expected ','");
            *pp = p;
            return;
        }
        p++;
        if (!source_parse_framebuffer_target(&p, &to)) {
            bc_set_error(cs, "Unsupported FRAMEBUFFER COPY destination");
            *pp = p;
            return;
        }
        source_skip_space(&p);
        if (*p == ',') {
            p++;
            source_skip_space(&p);
            if (*p == '"') {
                p++;
                if ((*p == 'B' || *p == 'b') && p[1] == '"') {
                    background = 1;
                    p += 2;
                } else {
                    bc_set_error(cs, "Unsupported FRAMEBUFFER COPY mode");
                    *pp = p;
                    return;
                }
            } else if (*p == 'B' || *p == 'b') {
                background = 1;
                p++;
            } else {
                bc_set_error(cs, "Unsupported FRAMEBUFFER COPY mode");
                *pp = p;
                return;
            }
        }
        aux[0] = BC_FB_OP_COPY;
        aux[1] = (uint8_t)from;
        aux[2] = (uint8_t)to;
        aux[3] = background;
        source_emit_syscall(cs, BC_SYS_GFX_FRAMEBUFFER, 0, aux, 4);
        *pp = p;
        return;
    }

    bc_set_error(cs, "Unsupported FRAMEBUFFER command");
    *pp = p;
}

static void source_compile_play(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    source_skip_space(&p);

    if (source_keyword(&p, "STOP")) {
        source_emit_syscall_noaux(cs, BC_SYS_PLAY_STOP, 0);
        *pp = p;
        return;
    }

    if (source_keyword(&p, "TONE")) {
        uint8_t argc = 2;
        uint8_t type;

        type = source_parse_expression(fe, cs, &p);
        source_emit_float_conversion(cs, type);
        if (!source_expect_char(cs, &p, ',', "Expected comma in PLAY TONE"))
            return;

        type = source_parse_expression(fe, cs, &p);
        source_emit_float_conversion(cs, type);

        source_skip_space(&p);
        if (*p == ',') {
            p++;
            type = source_parse_expression(fe, cs, &p);
            source_emit_int_conversion(cs, type);
            argc = 3;
        }

        source_emit_syscall_noaux(cs, BC_SYS_PLAY_TONE, argc);
        *pp = p;
        return;
    }

    bc_set_error(cs, "Unsupported PLAY command");
    *pp = p;
}

static void source_compile_pwm(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    source_skip_space(&p);

    if (source_keyword(&p, "SYNC")) {
        uint16_t present = 0;
        for (int i = 0; i < 12; i++) {
            source_skip_space(&p);
            if (*p != ',' && *p != '\0' && *p != '\'') {
                uint8_t type = source_parse_expression(fe, cs, &p);
                source_emit_float_conversion(cs, type);
                present |= (uint16_t)(1u << i);
            }
            source_skip_space(&p);
            if (*p == ',') {
                p++;
                continue;
            }
            break;
        }
        {
            uint8_t aux[2] = {(uint8_t)(present & 0xFF), (uint8_t)(present >> 8)};
            source_emit_syscall(cs, BC_SYS_PWM_SYNC, (uint8_t)__builtin_popcount((unsigned)present), aux, 2);
        }
        *pp = p;
        return;
    }

    {
        uint8_t type = source_parse_expression(fe, cs, &p);
        uint8_t present = 0;
        source_emit_int_conversion(cs, type);
        if (!source_expect_char(cs, &p, ',', "Expected comma in PWM")) {
            *pp = p;
            return;
        }
        source_skip_space(&p);
        if (source_keyword(&p, "OFF")) {
            source_emit_syscall_noaux(cs, BC_SYS_PWM_OFF, 1);
            *pp = p;
            return;
        }

        type = source_parse_expression(fe, cs, &p);
        source_emit_float_conversion(cs, type);
        if (!source_expect_char(cs, &p, ',', "Expected duty cycle after PWM frequency")) {
            *pp = p;
            return;
        }
        for (int slot = 0; slot < 4; slot++) {
            source_skip_space(&p);
            if (*p != ',' && *p != '\0' && *p != '\'') {
                type = source_parse_expression(fe, cs, &p);
                if (slot < 2)
                    source_emit_float_conversion(cs, type);
                else
                    source_emit_int_conversion(cs, type);
                present |= (uint8_t)(1u << slot);
            }
            source_skip_space(&p);
            if (slot == 3 || *p != ',')
                break;
            p++;
        }
        source_emit_syscall(cs, BC_SYS_PWM_CONFIG, (uint8_t)(2 + ((present & 0x01) ? 1 : 0) +
                                                             ((present & 0x02) ? 1 : 0) +
                                                             ((present & 0x04) ? 1 : 0) +
                                                             ((present & 0x08) ? 1 : 0)),
                            &present, 1);
        *pp = p;
    }
}

static void source_compile_servo(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    uint8_t present = 0;
    uint8_t type = source_parse_expression(fe, cs, &p);
    source_emit_int_conversion(cs, type);
    if (!source_expect_char(cs, &p, ',', "Expected comma in SERVO")) {
        *pp = p;
        return;
    }
    source_skip_space(&p);
    if (source_keyword(&p, "OFF")) {
        source_emit_syscall_noaux(cs, BC_SYS_PWM_OFF, 1);
        *pp = p;
        return;
    }
    for (int slot = 0; slot < 2; slot++) {
        source_skip_space(&p);
        if (*p != ',' && *p != '\0' && *p != '\'') {
            type = source_parse_expression(fe, cs, &p);
            source_emit_float_conversion(cs, type);
            present |= (uint8_t)(1u << slot);
        }
        source_skip_space(&p);
        if (slot == 1 || *p != ',')
            break;
        p++;
    }
    source_emit_syscall(cs, BC_SYS_SERVO, (uint8_t)(1 + ((present & 0x01) ? 1 : 0) +
                                                   ((present & 0x02) ? 1 : 0)),
                        &present, 1);
    *pp = p;
}

static void source_compile_setpin(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    int mode = 0;
    int option = VM_PIN_OPT_NONE;

    source_compile_pin_operand(fe, cs, &p);
    if (cs->has_error) {
        *pp = p;
        return;
    }
    if (!source_expect_char(cs, &p, ',', "Expected comma in SETPIN")) {
        *pp = p;
        return;
    }
    if (!source_parse_setpin_mode(&p, &mode)) {
        bc_set_error(cs, "Unsupported SETPIN mode");
        *pp = p;
        return;
    }

    source_skip_space(&p);
    if (*p == ',') {
        p++;
        source_skip_space(&p);
        if (source_keyword(&p, "PULLUP")) {
            option = VM_PIN_OPT_PULLUP;
        } else if (source_keyword(&p, "PULLDOWN")) {
            option = VM_PIN_OPT_PULLDOWN;
        } else {
            bc_set_error(cs, "Unsupported SETPIN option");
            *pp = p;
            return;
        }
    }

    {
        uint8_t aux[4];
        aux[0] = (uint8_t)(mode & 0xFF);
        aux[1] = (uint8_t)(((uint16_t)mode >> 8) & 0xFF);
        aux[2] = (uint8_t)(option & 0xFF);
        aux[3] = (uint8_t)(((uint16_t)option >> 8) & 0xFF);
        source_emit_syscall(cs, BC_SYS_SETPIN, 1, aux, 4);
    }
    *pp = p;
}

static void source_compile_open(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    int fnbr = 0;
    int mode = 0;
    uint8_t type;

    type = source_parse_expression(fe, cs, &p);
    if (cs->has_error) {
        *pp = p;
        return;
    }
    if (type != T_STR) {
        bc_set_error(cs, "OPEN requires string filename");
        *pp = p;
        return;
    }

    source_skip_space(&p);
    if (!source_keyword(&p, "FOR")) {
        bc_set_error(cs, "Expected FOR in OPEN");
        *pp = p;
        return;
    }
    source_skip_space(&p);
    if (source_keyword(&p, "INPUT")) {
        mode = VM_FILE_MODE_INPUT;
    } else if (source_keyword(&p, "OUTPUT")) {
        mode = VM_FILE_MODE_OUTPUT;
    } else if (source_keyword(&p, "APPEND")) {
        mode = VM_FILE_MODE_APPEND;
    } else {
        bc_set_error(cs, "Unsupported OPEN mode");
        *pp = p;
        return;
    }

    source_skip_space(&p);
    if (!source_keyword(&p, "AS")) {
        bc_set_error(cs, "Expected AS in OPEN");
        *pp = p;
        return;
    }
    if (!source_parse_file_number(cs, &p, &fnbr)) {
        *pp = p;
        return;
    }

    {
        uint8_t aux[3] = {(uint8_t)mode, (uint8_t)(fnbr & 0xFF), (uint8_t)(fnbr >> 8)};
        source_emit_syscall(cs, BC_SYS_FILE_OPEN, 1, aux, 3);
    }
    *pp = p;
}

static void source_compile_close(BCCompiler *cs, const char **pp) {
    const char *p = *pp;

    while (!cs->has_error) {
        int fnbr = 0;
        if (!source_parse_file_number(cs, &p, &fnbr)) {
            *pp = p;
            return;
        }
        {
            uint8_t aux[2] = {(uint8_t)(fnbr & 0xFF), (uint8_t)(fnbr >> 8)};
            source_emit_syscall(cs, BC_SYS_FILE_CLOSE, 0, aux, 2);
        }

        source_skip_space(&p);
        if (*p != ',') break;
        p++;
    }

    *pp = p;
}

static int source_parse_string_expression(BCSourceFrontend *fe, BCCompiler *cs, const char **pp,
                                          const char *msg) {
    uint8_t type = source_parse_expression(fe, cs, pp);
    if (cs->has_error) return 0;
    if (type != T_STR) {
        bc_set_error(cs, "%s", msg);
        return 0;
    }
    return 1;
}

static void source_compile_drive(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    if (!source_parse_string_expression(fe, cs, &p, "DRIVE requires string argument")) {
        *pp = p;
        return;
    }
    source_emit_syscall_noaux(cs, BC_SYS_FILE_DRIVE, 1);
    *pp = p;
}

static void source_compile_seek(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    int fnbr = 0;
    uint8_t type;

    if (!source_parse_file_number(cs, &p, &fnbr)) {
        *pp = p;
        return;
    }
    if (!source_expect_char(cs, &p, ',', "Expected ',' in SEEK")) {
        *pp = p;
        return;
    }
    type = source_parse_expression(fe, cs, &p);
    source_emit_int_conversion(cs, type);
    {
        uint8_t aux[2] = {(uint8_t)(fnbr & 0xFF), (uint8_t)(fnbr >> 8)};
        source_emit_syscall(cs, BC_SYS_FILE_SEEK, 1, aux, 2);
    }
    *pp = p;
}

static void source_compile_file_path_command(BCSourceFrontend *fe, BCCompiler *cs, const char **pp,
                                             uint16_t sysid, const char *msg) {
    const char *p = *pp;
    if (!source_parse_string_expression(fe, cs, &p, msg)) {
        *pp = p;
        return;
    }
    source_emit_syscall_noaux(cs, sysid, 1);
    *pp = p;
}

static void source_compile_rename(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    if (!source_parse_string_expression(fe, cs, &p, "RENAME requires string source")) {
        *pp = p;
        return;
    }
    source_skip_space(&p);
    if (!source_keyword(&p, "AS")) {
        bc_set_error(cs, "Expected AS in RENAME");
        *pp = p;
        return;
    }
    if (!source_parse_string_expression(fe, cs, &p, "RENAME requires string destination")) {
        *pp = p;
        return;
    }
    source_emit_syscall_noaux(cs, BC_SYS_FILE_RENAME, 2);
    *pp = p;
}

static void source_compile_copy(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    uint8_t mode = 0;

    source_skip_space(&p);
    if (source_keyword(&p, "A2A")) mode = 1;
    else if (source_keyword(&p, "A2B")) mode = 2;
    else if (source_keyword(&p, "B2A")) mode = 3;
    else if (source_keyword(&p, "B2B")) mode = 4;

    if (!source_parse_string_expression(fe, cs, &p, "COPY requires string source")) {
        *pp = p;
        return;
    }
    source_skip_space(&p);
    if (!source_keyword(&p, "TO")) {
        bc_set_error(cs, "Expected TO in COPY");
        *pp = p;
        return;
    }
    if (!source_parse_string_expression(fe, cs, &p, "COPY requires string destination")) {
        *pp = p;
        return;
    }
    source_emit_syscall(cs, BC_SYS_FILE_COPY, 2, &mode, 1);
    *pp = p;
}

static void source_compile_files(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    int has_pattern = 0;

    source_skip_space(&p);
    if (*p != '\0' && *p != '\'') {
        has_pattern = 1;
        if (!source_parse_string_expression(fe, cs, &p, "FILES requires string pattern")) {
            *pp = p;
            return;
        }
    }
    {
        uint8_t aux = (uint8_t)has_pattern;
        source_emit_syscall(cs, BC_SYS_FILE_FILES, (uint8_t)has_pattern, &aux, 1);
    }
    *pp = p;
}

static void source_compile_line_input(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    int fnbr = 0;
    char name[MAXVARLEN + 1];
    int name_len = 0;
    uint8_t vtype = 0;
    int is_local = 0;
    uint16_t slot;
    (void)fe;

    if (!source_parse_file_number(cs, &p, &fnbr)) {
        *pp = p;
        return;
    }
    if (!source_expect_char(cs, &p, ',', "Expected comma in LINE INPUT")) {
        *pp = p;
        return;
    }
    if (!source_parse_varname(&p, name, &name_len, &vtype)) {
        bc_set_error(cs, "Expected string variable in LINE INPUT");
        *pp = p;
        return;
    }
    if (vtype == 0) vtype = source_default_var_type(cs, name, name_len);
    if (vtype != T_STR) {
        bc_set_error(cs, "LINE INPUT requires string variable");
        *pp = p;
        return;
    }
    source_skip_space(&p);
    if (*p == '(') {
        bc_set_error(cs, "Unsupported LINE INPUT array target");
        *pp = p;
        return;
    }

    slot = source_resolve_var(cs, name, name_len, vtype, 1, &is_local);
    {
        uint8_t aux[5] = {(uint8_t)is_local,
                          (uint8_t)(slot & 0xFF), (uint8_t)(slot >> 8),
                          (uint8_t)(fnbr & 0xFF), (uint8_t)(fnbr >> 8)};
        source_emit_syscall(cs, BC_SYS_FILE_LINE_INPUT, 0, aux, 5);
    }
    *pp = p;
}

typedef struct {
    int present;
    uint8_t kind;
    uint8_t type;
    uint16_t slot;
} SourceGfxArg;

static int source_compile_expr_slice(BCSourceFrontend *fe, BCCompiler *cs,
                                     const char *start, const char *end,
                                     uint8_t *type_out) {
    while (start < end && (*start == ' ' || *start == '\t')) start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
    if (end <= start) return 0;
    if ((size_t)(end - start) > STRINGSIZE) {
        bc_set_error(cs, "Expression too long");
        return -1;
    }

    char expr[STRINGSIZE + 1];
    memcpy(expr, start, (size_t)(end - start));
    expr[end - start] = '\0';
    const char *p = expr;
    *type_out = source_parse_expression(fe, cs, &p);
    source_statement_end(cs, p);
    return cs->has_error ? -1 : 1;
}

static int source_compile_text_just_literal(BCCompiler *cs, const char *start, const char *end) {
    while (start < end && (*start == ' ' || *start == '\t')) start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
    if (end <= start || (size_t)(end - start) >= STRINGSIZE) return 0;
    for (const char *p = start; p < end; p++) {
        if (!isalpha((unsigned char)*p) && *p != ' ') return 0;
    }
    uint16_t idx = bc_add_constant_string(cs, (const uint8_t *)start, (int)(end - start));
    bc_emit_byte(cs, OP_PUSH_STR);
    bc_emit_u16(cs, idx);
    return 1;
}

static int source_try_parse_gfx_array_ref(BCCompiler *cs, SourceGfxArg *arg,
                                          const char *cmd_name,
                                          const char *start, const char *end) {
    char name[MAXVARLEN + 1];
    int name_len = 0;
    uint8_t type_hint = 0;
    uint8_t type = 0;
    uint16_t slot = 0xFFFF;
    int is_local = 0;
    int symbol_is_array = 0;

    while (start < end && (*start == ' ' || *start == '\t')) start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
    if (start >= end || !isnamestart((unsigned char)*start)) return 0;

    const char *p = start;
    if (!source_parse_varname(&p, name, &name_len, &type_hint)) return 0;
    source_skip_space(&p);
    if (p < end) {
        if (*p != '(') return 0;
        p++;
        source_skip_space(&p);
        if (p >= end || *p != ')') return 0;
        p++;
        source_skip_space(&p);
        if (p != end) return 0;
    }

    if (cs->current_subfun >= 0) {
        int loc = bc_find_local(cs, name, name_len);
        if (loc >= 0) {
            slot = (uint16_t)loc;
            type = cs->locals[loc].type;
            symbol_is_array = cs->locals[loc].is_array;
            is_local = 1;
        }
    }

    if (!is_local) {
        slot = bc_find_slot(cs, name, name_len);
        if (slot == 0xFFFF) {
            if (type_hint == 0) return 0;
            slot = bc_add_slot(cs, name, name_len, type_hint, 1);
            if (slot == 0xFFFF) return -1;
        }
        type = cs->slots[slot].type;
        symbol_is_array = cs->slots[slot].is_array;
    }

    if (!symbol_is_array) return 0;
    if ((type & (T_INT | T_NBR)) == 0 || (type & T_STR)) {
        bc_set_error(cs, "%s requires numeric array arguments", cmd_name);
        return -1;
    }

    arg->present = 1;
    arg->slot = slot;
    arg->type = type;
    if (is_local)
        arg->kind = (type == T_INT) ? BC_BOX_ARG_LOCAL_ARR_I : BC_BOX_ARG_LOCAL_ARR_F;
    else
        arg->kind = (type == T_INT) ? BC_BOX_ARG_GLOBAL_ARR_I : BC_BOX_ARG_GLOBAL_ARR_F;
    return 1;
}

static void source_emit_syscall(BCCompiler *cs, uint16_t sysid, uint8_t argc,
                                const uint8_t *aux, uint8_t auxlen) {
    bc_emit_byte(cs, OP_SYSCALL);
    bc_emit_u16(cs, sysid);
    bc_emit_byte(cs, argc);
    bc_emit_byte(cs, auxlen);
    for (uint8_t i = 0; i < auxlen; i++) {
        bc_emit_byte(cs, aux[i]);
    }
}

static void source_emit_syscall_noaux(BCCompiler *cs, uint16_t sysid, uint8_t argc) {
    source_emit_syscall(cs, sysid, argc, NULL, 0);
}

static uint8_t source_gfx_stack_argc(int max_args, SourceGfxArg *args) {
    uint8_t argc = 0;
    for (int i = 0; i < max_args; i++) {
        if (args[i].kind == BC_BOX_ARG_STACK) argc++;
    }
    return argc;
}

static void source_emit_gfx_native(BCCompiler *cs, uint16_t sysid, int max_args,
                                   int field_count, SourceGfxArg *args) {
    uint8_t argc = source_gfx_stack_argc(max_args, args);
    uint8_t aux[1 + BC_TEXT_ARG_COUNT * 3];
    int auxlen = 0;

    aux[auxlen++] = (uint8_t)field_count;
    for (int i = 0; i < max_args; i++) {
        aux[auxlen++] = args[i].kind;
        if (args[i].kind == BC_BOX_ARG_GLOBAL_ARR_I ||
            args[i].kind == BC_BOX_ARG_GLOBAL_ARR_F ||
            args[i].kind == BC_BOX_ARG_LOCAL_ARR_I ||
            args[i].kind == BC_BOX_ARG_LOCAL_ARR_F) {
            aux[auxlen++] = (uint8_t)(args[i].slot & 0xFF);
            aux[auxlen++] = (uint8_t)(args[i].slot >> 8);
        }
    }
    source_emit_syscall(cs, sysid, argc, aux, (uint8_t)auxlen);
}

static void source_compile_gfx_args(BCSourceFrontend *fe, BCCompiler *cs, const char **pp,
                                    const char *cmd_name, uint16_t sysid,
                                    int min_args, int max_args, int text_mode) {
    SourceGfxArg args[BC_TEXT_ARG_COUNT];
    const char *p = *pp;
    int field_count = 0;
    int idx = 0;

    memset(args, 0, sizeof(args));
    for (int i = 0; i < BC_TEXT_ARG_COUNT; i++) args[i].kind = BC_BOX_ARG_EMPTY;

    while (idx < max_args) {
        source_skip_space(&p);
        const char *start = p;
        int in_string = 0;
        int depth = 0;
        while (*p) {
            if (*p == '"') in_string = !in_string;
            else if (!in_string && *p == '(') depth++;
            else if (!in_string && *p == ')') depth--;
            else if (!in_string && depth == 0 && (*p == ',' || *p == '\'')) break;
            p++;
        }
        const char *end = p;

        uint8_t type = 0;
        int present = 0;
        int array_rc = source_try_parse_gfx_array_ref(cs, &args[idx], cmd_name, start, end);
        if (array_rc < 0) return;
        if (array_rc > 0) {
            present = 1;
            type = args[idx].type;
        }
        if (text_mode && idx == 3) {
            int literal = source_compile_text_just_literal(cs, start, end);
            if (literal < 0) return;
            if (literal > 0) {
                present = 1;
                type = T_STR;
            }
        }
        if (!present) {
            int rc = source_compile_expr_slice(fe, cs, start, end, &type);
            if (rc < 0) return;
            present = rc > 0;
        }

        if (present) {
            int wants_string = text_mode && (idx == 2 || idx == 3);
            int wants_numeric = !wants_string;
            if (wants_string && type != T_STR) {
                bc_set_error(cs, "%s requires string arguments", cmd_name);
                return;
            }
            if (wants_numeric && (((type & (T_INT | T_NBR)) == 0) || (type & T_STR))) {
                bc_set_error(cs, "%s requires numeric arguments", cmd_name);
                return;
            }
            args[idx].present = 1;
            if (args[idx].kind == BC_BOX_ARG_EMPTY) args[idx].kind = BC_BOX_ARG_STACK;
            args[idx].type = type;
        }
        field_count = idx + 1;
        idx++;

        if (*p == ',') {
            p++;
            continue;
        }
        break;
    }

    if (field_count < min_args || field_count > max_args) {
        bc_set_error(cs, "Invalid %s argument count", cmd_name);
        *pp = p;
        return;
    }
    source_emit_gfx_native(cs, sysid, max_args, field_count, args);
    *pp = p;
}

static void source_compile_polygon(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    SourceGfxArg args[BC_POLYGON_ARG_COUNT];
    const char *p = *pp;
    int field_count = 0;

    memset(args, 0, sizeof(args));
    for (int i = 0; i < BC_POLYGON_ARG_COUNT; i++) args[i].kind = BC_BOX_ARG_EMPTY;

    for (int idx = 0; idx < BC_POLYGON_ARG_COUNT; idx++) {
        source_skip_space(&p);
        const char *start = p;
        int in_string = 0;
        int depth = 0;
        while (*p) {
            if (*p == '"') in_string = !in_string;
            else if (!in_string && *p == '(') depth++;
            else if (!in_string && *p == ')') depth--;
            else if (!in_string && depth == 0 && (*p == ',' || *p == '\'')) break;
            p++;
        }
        const char *end = p;
        uint8_t type = 0;
        int present = 0;
        int array_rc = source_try_parse_gfx_array_ref(cs, &args[idx], "POLYGON", start, end);
        if (array_rc < 0) return;
        if (idx == 1 || idx == 2) {
            if (array_rc <= 0) {
                bc_set_error(cs, "POLYGON requires numeric array arguments");
                return;
            }
            present = 1;
            type = args[idx].type;
        } else if (array_rc > 0) {
            present = 1;
            type = args[idx].type;
        } else {
            int rc = source_compile_expr_slice(fe, cs, start, end, &type);
            if (rc < 0) return;
            present = rc > 0;
        }

        if (!present) {
            if (idx < 3) {
                bc_set_error(cs, "Argument count");
                return;
            }
            break;
        }
        if (((type & (T_INT | T_NBR)) == 0) || (type & T_STR)) {
            bc_set_error(cs, "POLYGON requires numeric arguments");
            return;
        }
        args[idx].present = 1;
        if (args[idx].kind == BC_BOX_ARG_EMPTY) args[idx].kind = BC_BOX_ARG_STACK;
        args[idx].type = type;
        field_count = idx + 1;

        if (*p == ',') {
            p++;
            continue;
        }
        break;
    }

    if (field_count < 3 || field_count > BC_POLYGON_ARG_COUNT) {
        bc_set_error(cs, "Argument count");
        *pp = p;
        return;
    }
    source_emit_gfx_native(cs, BC_SYS_GFX_POLYGON, BC_POLYGON_ARG_COUNT, field_count, args);
    *pp = p;
}

static void source_compile_cls(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    source_skip_space(&p);
    int has_arg = 0;
    if (*p && *p != '\'') {
        uint8_t type = source_parse_expression(fe, cs, &p);
        if (((type & (T_INT | T_NBR)) == 0) || (type & T_STR))
            bc_set_error(cs, "CLS requires numeric arguments");
        has_arg = 1;
    }
    {
        uint8_t aux = (uint8_t)has_arg;
        source_emit_syscall(cs, BC_SYS_GFX_CLS, (uint8_t)has_arg, &aux, 1);
    }
    *pp = p;
}

static void source_compile_select_case(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    source_skip_space(&p);
    if (!source_keyword(&p, "CASE")) {
        bc_set_error(cs, "Expected CASE after SELECT");
        *pp = p;
        return;
    }
    uint8_t type = source_parse_expression(fe, cs, &p);
    uint16_t slot = source_alloc_hidden_slot(cs, type);
    source_emit_store_converted(cs, slot, type, type, 0);

    bc_nest_push(cs, NEST_SELECT);
    BCNestEntry *ne = bc_nest_top(cs);
    if (ne) {
        ne->select_slot = slot;
        ne->select_type = type;
        ne->addr1 = 0xFFFFFFFF;
    }
    *pp = p;
}

static void source_compile_case(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    BCNestEntry *ne = bc_nest_find(cs, NEST_SELECT);
    if (!ne) {
        bc_set_error(cs, "CASE without matching SELECT CASE");
        return;
    }

    if (ne->addr1 != 0xFFFFFFFF) {
        uint32_t end_jmp = source_emit_jmp_placeholder(cs, OP_JMP);
        if (ne->case_end_count < 32) ne->case_end_fixups[ne->case_end_count++] = end_jmp;
        source_patch_jmp_here(cs, ne->addr1);
        ne->addr1 = 0xFFFFFFFF;
    }

    const char *p = *pp;
    source_skip_space(&p);
    if (source_keyword(&p, "ELSE")) {
        ne->has_else = 1;
        *pp = p;
        return;
    }

    uint32_t body_patches[32];
    int body_patch_count = 0;
    while (!cs->has_error) {
        bc_emit_load_var(cs, ne->select_slot, ne->select_type, 0);
        uint32_t right_start = cs->code_len;
        uint8_t rhs = source_parse_expression(fe, cs, &p);
        source_skip_space(&p);
        if (source_keyword(&p, "TO")) {
            source_emit_compare(cs, ne->select_type, rhs, right_start, 'g');
            uint32_t low_fail = source_emit_jmp_placeholder(cs, OP_JZ);

            bc_emit_load_var(cs, ne->select_slot, ne->select_type, 0);
            uint32_t high_start = cs->code_len;
            uint8_t high = source_parse_expression(fe, cs, &p);
            source_emit_compare(cs, ne->select_type, high, high_start, 'l');
            if (body_patch_count < 32)
                body_patches[body_patch_count++] = source_emit_jmp_placeholder(cs, OP_JNZ);
            source_patch_jmp_here(cs, low_fail);
        } else {
            source_emit_compare(cs, ne->select_type, rhs, right_start, '=');
            if (body_patch_count < 32)
                body_patches[body_patch_count++] = source_emit_jmp_placeholder(cs, OP_JNZ);
        }

        source_skip_space(&p);
        if (*p != ',') break;
        p++;
    }

    ne->addr1 = source_emit_jmp_placeholder(cs, OP_JMP);
    for (int i = 0; i < body_patch_count; i++)
        source_patch_jmp_here(cs, body_patches[i]);
    *pp = p;
}

static void source_compile_end_select(BCCompiler *cs) {
    BCNestEntry *ne = bc_nest_find(cs, NEST_SELECT);
    if (!ne) {
        bc_set_error(cs, "END SELECT without matching SELECT CASE");
        return;
    }
    if (ne->addr1 != 0xFFFFFFFF) source_patch_jmp_here(cs, ne->addr1);
    for (int i = 0; i < ne->case_end_count; i++)
        source_patch_jmp_here(cs, ne->case_end_fixups[i]);
    bc_nest_pop(cs);
}

static const char *source_find_keyword_outside_string(const char *p, const char *kw) {
    const char *base = p;
    int in_string = 0;
    size_t len = strlen(kw);
    for (; *p; p++) {
        if (*p == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) continue;
        if (strncasecmp(p, kw, len) == 0 &&
            (p == base || !isnamechar((unsigned char)p[-1])) &&
            !isnamechar((unsigned char)p[len])) {
            return p;
        }
    }
    return NULL;
}

static void source_compile_statement_list(BCSourceFrontend *fe, BCCompiler *cs, const char *text) {
    const char *p = text;
    while (*p && !cs->has_error) {
        source_skip_space(&p);
        if (*p == '\0' || *p == '\'') break;

        const char *start = p;
        const char *kw_probe = p;
        int if_statement = source_keyword(&kw_probe, "IF");
        int in_string = 0;
        while (*p) {
            if (*p == '"') in_string = !in_string;
            if (!in_string && ((!if_statement && *p == ':') || *p == '\'')) break;
            p++;
        }

        char stmt[STRINGSIZE + 1];
        size_t len = (size_t)(p - start);
        if (len > STRINGSIZE) len = STRINGSIZE;
        memcpy(stmt, start, len);
        stmt[len] = '\0';
        source_compile_statement(fe, cs, stmt);

        if (*p == ':') {
            p++;
            continue;
        }
        break;
    }
}

static void source_compile_if(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    const char *p = *pp;
    const char *then_kw = source_find_keyword_outside_string(p, "THEN");
    if (!then_kw) {
        bc_set_error(cs, "IF without THEN");
        *pp = p;
        return;
    }

    char cond[STRINGSIZE + 1];
    size_t cond_len = (size_t)(then_kw - p);
    if (cond_len > STRINGSIZE) cond_len = STRINGSIZE;
    memcpy(cond, p, cond_len);
    cond[cond_len] = '\0';
    const char *cond_p = cond;
    uint8_t cond_type = source_parse_expression(fe, cs, &cond_p);
    if (cs->has_error) {
        *pp = p;
        return;
    }
    if (cond_type == T_STR) {
        bc_set_error(cs, "IF requires a numeric condition");
        *pp = p;
        return;
    }
    source_statement_end(cs, cond_p);
    if (cs->has_error) {
        *pp = p;
        return;
    }

    uint32_t false_patch = source_emit_jmp_placeholder(cs, OP_JZ);

    const char *then_start = then_kw + 4;
    if (source_line_empty_or_comment(then_start)) {
        bc_nest_push(cs, NEST_IF);
        BCNestEntry *ne = bc_nest_top(cs);
        if (ne) {
            ne->addr1 = false_patch;
            ne->addr2 = 0xFFFFFFFF;
        }
        *pp = then_start + strlen(then_start);
        return;
    }

    const char *else_kw = source_find_keyword_outside_string(then_start, "ELSE");
    char then_stmt[STRINGSIZE + 1];
    size_t then_len = else_kw ? (size_t)(else_kw - then_start) : strlen(then_start);
    if (then_len > STRINGSIZE) then_len = STRINGSIZE;
    memcpy(then_stmt, then_start, then_len);
    then_stmt[then_len] = '\0';
    source_compile_statement_list(fe, cs, then_stmt);
    if (cs->has_error) {
        *pp = then_start;
        return;
    }

    if (else_kw) {
        uint32_t end_patch = source_emit_jmp_placeholder(cs, OP_JMP);
        source_patch_jmp_here(cs, false_patch);
        source_compile_statement_list(fe, cs, else_kw + 4);
        source_patch_jmp_here(cs, end_patch);
        *pp = else_kw + strlen(else_kw);
        return;
    }

    source_patch_jmp_here(cs, false_patch);
    *pp = then_start + strlen(then_start);
}

static void source_compile_elseif(BCSourceFrontend *fe, BCCompiler *cs, const char **pp) {
    BCNestEntry *ne = bc_nest_top(cs);
    if (!ne || ne->type != NEST_IF) {
        bc_set_error(cs, "ELSEIF without matching IF");
        return;
    }

    uint32_t end_patch = source_emit_jmp_placeholder(cs, OP_JMP);
    if (ne->exit_fixup_count < 64) ne->exit_fixups[ne->exit_fixup_count++] = end_patch;
    if (ne->addr1 != 0xFFFFFFFF) source_patch_jmp_here(cs, ne->addr1);

    const char *p = *pp;
    const char *then_kw = source_find_keyword_outside_string(p, "THEN");
    if (!then_kw) {
        bc_set_error(cs, "ELSEIF without THEN");
        *pp = p;
        return;
    }

    char cond[STRINGSIZE + 1];
    size_t cond_len = (size_t)(then_kw - p);
    if (cond_len > STRINGSIZE) cond_len = STRINGSIZE;
    memcpy(cond, p, cond_len);
    cond[cond_len] = '\0';
    const char *cond_p = cond;
    uint8_t cond_type = source_parse_expression(fe, cs, &cond_p);
    if (cond_type == T_STR) bc_set_error(cs, "ELSEIF requires a numeric condition");
    source_statement_end(cs, cond_p);
    if (cs->has_error) {
        *pp = p;
        return;
    }

    ne->addr1 = source_emit_jmp_placeholder(cs, OP_JZ);
    const char *then_start = then_kw + 4;
    if (source_line_empty_or_comment(then_start)) {
        *pp = then_start + strlen(then_start);
        return;
    }

    source_compile_statement(fe, cs, then_start);
    *pp = then_start + strlen(then_start);
}

static void source_compile_else(BCCompiler *cs, const char **pp) {
    BCNestEntry *ne = bc_nest_top(cs);
    if (!ne || ne->type != NEST_IF) {
        bc_set_error(cs, "ELSE without matching IF");
        return;
    }
    uint32_t end_patch = source_emit_jmp_placeholder(cs, OP_JMP);
    if (ne->exit_fixup_count < 64) ne->exit_fixups[ne->exit_fixup_count++] = end_patch;
    if (ne->addr1 != 0xFFFFFFFF) source_patch_jmp_here(cs, ne->addr1);
    ne->addr1 = 0xFFFFFFFF;
    ne->has_else = 1;
    const char *p = *pp;
    source_skip_space(&p);
    *pp = p;
}

static void source_compile_endif(BCCompiler *cs) {
    BCNestEntry *ne = bc_nest_top(cs);
    if (!ne || ne->type != NEST_IF) {
        bc_set_error(cs, "ENDIF without matching IF");
        return;
    }
    if (ne->addr1 != 0xFFFFFFFF) source_patch_jmp_here(cs, ne->addr1);
    for (int i = 0; i < ne->exit_fixup_count; i++)
        source_patch_jmp_here(cs, ne->exit_fixups[i]);
    bc_nest_pop(cs);
}

static void source_compile_statement(BCSourceFrontend *fe, BCCompiler *cs, const char *stmt) {
    const char *p = stmt;
    source_skip_space(&p);

    if (*p == '?') {
        p++;
        source_compile_print(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "PRINT")) {
        source_compile_print(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "OPTION")) {
        return;
    }

    if (source_keyword(&p, "CONST")) {
        source_compile_const(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "DIM")) {
        source_compile_dim(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "DATA")) {
        source_compile_data(cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "READ")) {
        source_compile_read(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "RESTORE")) {
        bc_emit_byte(cs, OP_RESTORE);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "INC")) {
        source_compile_inc(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "FASTGFX")) {
        source_compile_fastgfx(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "FRAMEBUFFER")) {
        source_compile_framebuffer(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "SAVE")) {
        source_skip_space(&p);
        if (!source_keyword(&p, "IMAGE")) {
            bc_set_error(cs, "Unsupported SAVE form");
            return;
        }
        while (*p && *p != '\'') p++;
        return;
    }

    if (source_keyword(&p, "RANDOMIZE")) {
        uint8_t type = source_parse_expression(fe, cs, &p);
        source_emit_int_conversion(cs, type);
        bc_emit_byte(cs, OP_RANDOMIZE);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "PAUSE")) {
        uint8_t type = source_parse_expression(fe, cs, &p);
        source_emit_int_conversion(cs, type);
        bc_emit_byte(cs, OP_PAUSE);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "PLAY")) {
        source_compile_play(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "SETPIN")) {
        source_compile_setpin(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "PWM")) {
        source_compile_pwm(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "SERVO")) {
        source_compile_servo(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "ERROR")) {
        source_skip_space(&p);
        if (*p == '\0' || *p == '\'') {
            bc_emit_byte(cs, OP_ERROR_EMPTY);
        } else {
            uint8_t type = source_parse_expression(fe, cs, &p);
            if (type != T_STR) bc_set_error(cs, "ERROR requires a string argument");
            bc_emit_byte(cs, OP_ERROR_S);
        }
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "OPEN")) {
        source_compile_open(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "DRIVE")) {
        source_compile_drive(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "CLOSE")) {
        source_compile_close(cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "FILES")) {
        source_compile_files(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "SEEK")) {
        source_compile_seek(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "MKDIR")) {
        source_compile_file_path_command(fe, cs, &p, BC_SYS_FILE_MKDIR, "MKDIR requires string path");
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "CHDIR")) {
        source_compile_file_path_command(fe, cs, &p, BC_SYS_FILE_CHDIR, "CHDIR requires string path");
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "RMDIR")) {
        source_compile_file_path_command(fe, cs, &p, BC_SYS_FILE_RMDIR, "RMDIR requires string path");
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "KILL")) {
        source_compile_file_path_command(fe, cs, &p, BC_SYS_FILE_KILL, "KILL requires string path");
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "RENAME")) {
        source_compile_rename(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "COPY")) {
        source_compile_copy(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "RUN")) {
        source_skip_space(&p);
        if (*p == '\0' || *p == '\'') {
            bc_set_error(cs, "RUN requires a filename");
        } else {
            if (!source_parse_string_expression(fe, cs, &p, "RUN requires string filename")) {
                /* error already set */
            } else {
                source_emit_syscall(cs, BC_SYS_RUN, 1, NULL, 0);
            }
        }
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "CLS")) {
        source_compile_cls(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "COLOUR") || source_keyword(&p, "COLOR")) {
        uint8_t type;
        int argc = 0;
        source_skip_space(&p);
        if (*p == '\0' || *p == '\'') {
            bc_set_error(cs, "Argument count");
            return;
        }
        type = source_parse_expression(fe, cs, &p);
        if (((type & (T_INT | T_NBR)) == 0) || (type & T_STR)) {
            bc_set_error(cs, "COLOUR requires numeric arguments");
            return;
        }
        argc = 1;
        source_skip_space(&p);
        if (*p == ',') {
            p++;
            type = source_parse_expression(fe, cs, &p);
            if (((type & (T_INT | T_NBR)) == 0) || (type & T_STR)) {
                bc_set_error(cs, "COLOUR requires numeric arguments");
                return;
            }
            argc = 2;
        }
        source_emit_syscall_noaux(cs, BC_SYS_GFX_COLOUR, (uint8_t)argc);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "FONT")) {
        uint8_t type;
        int argc = 0;
        source_skip_space(&p);
        if (*p == '#') p++;
        if (*p == '\0' || *p == '\'') {
            bc_set_error(cs, "Argument count");
            return;
        }
        type = source_parse_expression(fe, cs, &p);
        if (((type & (T_INT | T_NBR)) == 0) || (type & T_STR)) {
            bc_set_error(cs, "FONT requires numeric arguments");
            return;
        }
        argc = 1;
        source_skip_space(&p);
        if (*p == ',') {
            p++;
            type = source_parse_expression(fe, cs, &p);
            if (((type & (T_INT | T_NBR)) == 0) || (type & T_STR)) {
                bc_set_error(cs, "FONT requires numeric arguments");
                return;
            }
            argc = 2;
        }
        source_emit_syscall_noaux(cs, BC_SYS_GFX_FONT, (uint8_t)argc);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "BOX")) {
        source_compile_gfx_args(fe, cs, &p, "BOX", BC_SYS_GFX_BOX,
                                4, BC_BOX_ARG_COUNT, 0);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "RBOX")) {
        source_compile_gfx_args(fe, cs, &p, "RBOX", BC_SYS_GFX_RBOX,
                                4, BC_BOX_ARG_COUNT, 0);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "ARC")) {
        source_compile_gfx_args(fe, cs, &p, "ARC", BC_SYS_GFX_ARC,
                                6, BC_BOX_ARG_COUNT, 0);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "TRIANGLE")) {
        source_compile_gfx_args(fe, cs, &p, "TRIANGLE", BC_SYS_GFX_TRIANGLE,
                                6, BC_TRIANGLE_ARG_COUNT, 0);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "POLYGON")) {
        source_compile_polygon(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "CIRCLE")) {
        source_compile_gfx_args(fe, cs, &p, "CIRCLE", BC_SYS_GFX_CIRCLE,
                                3, BC_BOX_ARG_COUNT, 0);
        source_statement_end(cs, p);
        return;
    }

    {
        const char *q = p;
        if (source_keyword(&q, "LINE")) {
            const char *r = q;
            source_skip_space(&r);
            if (source_keyword(&r, "INPUT")) {
                p = r;
                source_compile_line_input(fe, cs, &p);
                source_statement_end(cs, p);
                return;
            }
        }
    }

    if (source_keyword(&p, "LINE")) {
        source_compile_gfx_args(fe, cs, &p, "LINE", BC_SYS_GFX_LINE,
                                2, BC_LINE_ARG_COUNT, 0);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "PIXEL")) {
        source_compile_gfx_args(fe, cs, &p, "PIXEL", BC_SYS_GFX_PIXEL,
                                2, BC_PIXEL_ARG_COUNT, 0);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "TEXT")) {
        source_compile_gfx_args(fe, cs, &p, "TEXT", BC_SYS_GFX_TEXT,
                                3, BC_TEXT_ARG_COUNT, 1);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "SELECT")) {
        source_compile_select_case(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "CASE")) {
        source_compile_case(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "LOCAL") || source_keyword(&p, "STATIC")) {
        source_compile_local(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "SUB")) {
        source_compile_sub(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "FUNCTION")) {
        source_compile_function(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "ELSEIF")) {
        source_compile_elseif(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "ELSE")) {
        const char *q = p;
        source_skip_space(&q);
        if (source_keyword(&q, "IF")) {
            source_compile_elseif(fe, cs, &q);
            source_statement_end(cs, q);
            return;
        }
        source_compile_else(cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "ENDIF") || source_keyword(&p, "END IF")) {
        source_compile_endif(cs);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "END")) {
        source_skip_space(&p);
        if (source_keyword(&p, "SUB")) {
            source_compile_end_sub(cs);
            source_statement_end(cs, p);
            return;
        }
        if (source_keyword(&p, "FUNCTION")) {
            source_compile_end_function(cs);
            source_statement_end(cs, p);
            return;
        }
        if (source_keyword(&p, "SELECT")) {
            source_compile_end_select(cs);
            source_statement_end(cs, p);
            return;
        }
        bc_emit_byte(cs, OP_END);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "DO")) {
        source_compile_do(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "LOOP")) {
        source_compile_loop(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "EXIT")) {
        source_compile_exit(cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "IF")) {
        source_compile_if(fe, cs, &p);
        return;
    }

    if (source_keyword(&p, "FOR")) {
        source_compile_for(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "NEXT")) {
        source_compile_next(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "GOTO")) {
        source_compile_goto(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "GOSUB")) {
        source_compile_gosub(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "RETURN")) {
        bc_emit_byte(cs, OP_RETURN);
        source_statement_end(cs, p);
        return;
    }

    if (source_keyword(&p, "LET")) {
        source_compile_assignment(fe, cs, &p);
        source_statement_end(cs, p);
        return;
    }

    if (isnamestart((unsigned char)*p)) {
        const char *probe = p;
        char name[MAXVARLEN + 1];
        int name_len = 0;
        uint8_t type = 0;
        if (source_parse_varname(&probe, name, &name_len, &type)) {
            source_skip_space(&probe);
            if (*probe == '=') {
                source_compile_assignment(fe, cs, &p);
                source_statement_end(cs, p);
                return;
            }
            const char *after_name = probe;
            if (*probe == '(') {
                int depth = 0;
                do {
                    if (*probe == '(') depth++;
                    else if (*probe == ')') depth--;
                    else if (*probe == '\0') break;
                    probe++;
                } while (depth > 0);
                source_skip_space(&probe);
                if (*probe == '=') {
                    source_compile_assignment(fe, cs, &p);
                    source_statement_end(cs, p);
                    return;
                }
            }

            int sf_idx = bc_find_subfun(cs, name, name_len);
            if (sf_idx >= 0 && cs->subfuns[sf_idx].return_type == 0) {
                p = after_name;
                int nargs = source_compile_call_args(fe, cs, &p, 0);
                if (!cs->has_error) {
                    bc_emit_byte(cs, OP_CALL_SUB);
                    bc_emit_u16(cs, (uint16_t)sf_idx);
                    bc_emit_byte(cs, (uint8_t)nargs);
                }
                source_statement_end(cs, p);
                return;
            }
        }
    }

    /* Unsupported command: bridge to interpreter.
     * Tokenize the source statement and embed the tokenized form
     * in the bytecode stream so the VM can hand it to the interpreter. */
    {
        unsigned char saved_inpbuf[STRINGSIZE];
        unsigned char saved_tknbuf[STRINGSIZE];
        memcpy(saved_inpbuf, inpbuf, STRINGSIZE);
        memcpy(saved_tknbuf, tknbuf, STRINGSIZE);

        /* Copy statement into inpbuf for tokenise() */
        size_t slen = strlen(stmt);
        if (slen >= STRINGSIZE) slen = STRINGSIZE - 1;
        memcpy(inpbuf, stmt, slen);
        inpbuf[slen] = 0;

        tokenise(1);  /* console mode: no T_NEWLINE prefix */

        /* tknbuf now has: cmd_token(2 bytes) + tokenized args + 0x00 terminator.
         * Find the length of the tokenized form. */
        unsigned char *tp = tknbuf;
        while (*tp) {
            if (*tp == T_LINENBR) { tp += 3; continue; }
            tp++;
        }
        uint16_t tok_len = (uint16_t)(tp - tknbuf);

        if (tok_len < 2) {
            bc_set_error(cs, "Unsupported source command near: %.24s", p);
        } else {
            bc_emit_byte(cs, OP_BRIDGE_CMD);
            bc_emit_u16(cs, tok_len);
            for (uint16_t i = 0; i < tok_len; i++)
                bc_emit_byte(cs, tknbuf[i]);
        }

        memcpy(inpbuf, saved_inpbuf, STRINGSIZE);
        memcpy(tknbuf, saved_tknbuf, STRINGSIZE);
    }
}

static void source_compile_line(BCSourceFrontend *fe, BCCompiler *cs, const char *line) {
    const char *p = line;
    source_skip_space(&p);

    int explicit_line = 0;
    if (isdigit((unsigned char)*p)) {
        char *end = NULL;
        long n = strtol(p, &end, 10);
        if (end != p) {
            explicit_line = (int)n;
            p = end;
            source_skip_space(&p);
        }
    }

    if (source_line_empty_or_comment(p)) return;

    int line_no = explicit_line > 0 ? explicit_line : fe->line_no;
    cs->current_line = line_no;
    bc_add_linemap_entry(cs, (uint16_t)line_no, cs->code_len);
    bc_emit_byte(cs, OP_LINE);
    bc_emit_u16(cs, (uint16_t)line_no);

    while (*p && !cs->has_error) {
        source_skip_space(&p);
        if (*p == '\0' || *p == '\'') break;

        const char *start = p;
        const char *kw_probe = p;
        int if_statement = source_keyword(&kw_probe, "IF");
        int in_string = 0;
        while (*p) {
            if (*p == '"') in_string = !in_string;
            if (!in_string && ((!if_statement && *p == ':') || *p == '\'')) break;
            p++;
        }

        char stmt[STRINGSIZE + 1];
        size_t len = (size_t)(p - start);
        if (len > STRINGSIZE) len = STRINGSIZE;
        memcpy(stmt, start, len);
        stmt[len] = '\0';
        source_compile_statement(fe, cs, stmt);

        if (*p == ':') {
            p++;
            continue;
        }
        break;
    }
}

static void source_skip_parenthesized(const char **pp) {
    const char *p = *pp;
    if (*p != '(') return;

    int depth = 0;
    int in_string = 0;
    while (*p) {
        if (*p == '"') {
            in_string = !in_string;
        } else if (!in_string) {
            if (*p == '(') depth++;
            else if (*p == ')') {
                depth--;
                if (depth == 0) {
                    p++;
                    break;
                }
            }
        }
        p++;
    }
    *pp = p;
}

static void source_predeclare_line(BCCompiler *cs, const char *line, int line_no) {
    const char *p = line;
    source_skip_space(&p);

    if (isdigit((unsigned char)*p)) {
        char *end = NULL;
        (void)strtol(p, &end, 10);
        if (end != p) {
            p = end;
            source_skip_space(&p);
        }
    }
    if (source_line_empty_or_comment(p)) return;

    cs->current_line = line_no;
    if (source_keyword(&p, "SUB")) {
        source_skip_space(&p);
        const char *name_start = p;
        char name[MAXVARLEN + 1];
        int name_len = 0;
        uint8_t type = 0;
        if (source_parse_varname(&p, name, &name_len, &type) && type == 0) {
            (void)source_get_or_create_subfun(cs, name_start, name_len, 0);
        }
        return;
    }

    if (source_keyword(&p, "FUNCTION")) {
        source_skip_space(&p);
        const char *name_start = p;
        char name[MAXVARLEN + 1];
        int name_len = 0;
        uint8_t ret_type = 0;
        if (!source_parse_varname(&p, name, &name_len, &ret_type)) return;

        int has_suffix = (ret_type != 0);
        int sf_name_len = has_suffix ? name_len - 1 : name_len;
        if (ret_type == 0) ret_type = T_NBR;

        source_skip_space(&p);
        if (*p == '(') source_skip_parenthesized(&p);
        uint8_t as_type = source_parse_as_type_clause(&p);
        if (as_type != 0 && !has_suffix) ret_type = as_type;

        (void)source_get_or_create_subfun(cs, name_start, sf_name_len, ret_type);
    }
}

static void source_update_continuation_setting(const char *line, unsigned char *continuation) {
    const char *p = line;

    if (!continuation) return;
    source_skip_space(&p);
    if (isdigit((unsigned char)*p)) {
        char *end = NULL;
        (void)strtol(p, &end, 10);
        if (end != p) {
            p = end;
            source_skip_space(&p);
        }
    }
    if (!source_keyword(&p, "OPTION")) return;
    source_skip_space(&p);
    if (!source_keyword(&p, "CONTINUATION")) return;
    source_skip_space(&p);
    if (!source_keyword(&p, "LINES")) return;
    source_skip_space(&p);
    if (source_keyword(&p, "ON") || source_keyword(&p, "ENABLE")) {
        *continuation = '_';
    } else if (source_keyword(&p, "OFF") || source_keyword(&p, "DISABLE")) {
        *continuation = 0;
    }
}

static int source_read_logical_line(const char **pp, char *line, size_t line_cap,
                                    int *physical_line_io, int *line_no_out,
                                    unsigned char *continuation) {
    const char *p = *pp;
    size_t out_len = 0;
    int line_no = *physical_line_io;

    if (*p == '\0') return 0;
    line[0] = '\0';

    while (*p) {
        const char *start = p;
        size_t len;
        while (*p && *p != '\n' && *p != '\r') p++;
        len = (size_t)(p - start);
        if (out_len + len > line_cap - 1) len = (line_cap - 1) - out_len;
        memcpy(line + out_len, start, len);
        out_len += len;
        line[out_len] = '\0';

        if (*p == '\r' && p[1] == '\n') p += 2;
        else if (*p == '\n' || *p == '\r') p++;

        (*physical_line_io)++;
        if (*continuation && out_len >= 2 &&
            line[out_len - 2] == ' ' && line[out_len - 1] == *continuation) {
            out_len -= 2;
            line[out_len] = '\0';
            continue;
        }
        break;
    }

    *pp = p;
    *line_no_out = line_no;
    source_update_continuation_setting(line, continuation);
    return 1;
}

static void source_predeclare_subfuns(BCCompiler *cs, const char *source) {
    int physical_line = 1;
    int line_no = 1;
    unsigned char continuation = 0;
    const char *p = source;
    char line[STRINGSIZE + 1];

    while (!cs->has_error &&
           source_read_logical_line(&p, line, sizeof(line), &physical_line, &line_no, &continuation)) {
        source_predeclare_line(cs, line, line_no);
    }
}

int bc_compile_source(BCCompiler *cs, const char *source, const char *source_name) {
    BCSourceFrontend fe;
    (void)source_name;
    fe.line_no = 1;

    source_predeclare_subfuns(cs, source);
    if (cs->has_error) return -1;

    const char *p = source;
    int physical_line = 1;
    unsigned char continuation = 0;
    while (!cs->has_error) {
        char line[STRINGSIZE + 1];
        if (!source_read_logical_line(&p, line, sizeof(line), &physical_line, &fe.line_no, &continuation))
            break;
        source_compile_line(&fe, cs, line);
    }

    if (cs->has_error) {
        if (cs->error_line == 0) cs->error_line = fe.line_no;
        return -1;
    }

    bc_emit_byte(cs, OP_END);
    bc_resolve_fixups(cs);
    return cs->has_error ? -1 : 0;
}
