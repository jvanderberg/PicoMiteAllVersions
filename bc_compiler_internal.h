/*
 * bc_compiler_internal.h — Internal API shared between compiler modules
 */
#ifndef __BC_COMPILER_INTERNAL_H
#define __BC_COMPILER_INTERNAL_H

#include "bytecode.h"

/* Token stream navigation helpers */
extern unsigned char tokenTHEN, tokenELSE, tokenGOTO, tokenEQUAL, tokenTO, tokenSTEP;
extern unsigned char tokenWHILE, tokenUNTIL, tokenGOSUB, tokenAS, tokenFOR;
extern unsigned short cmdIF, cmdENDIF, cmdEND_IF, cmdELSEIF, cmdELSE_IF, cmdELSE;
extern unsigned short cmdSELECT_CASE, cmdFOR, cmdNEXT, cmdWHILE, cmdENDSUB, cmdENDFUNCTION;
extern unsigned short cmdLOCAL, cmdSTATIC, cmdCASE, cmdDO, cmdLOOP, cmdCASE_ELSE, cmdEND_SELECT;
extern unsigned short cmdSUB, cmdFUN, cmdCSUB, cmdIRET, cmdComment, cmdEndComment;
extern const struct s_tokentbl tokentbl[];
extern const struct s_tokentbl commandtbl[];
extern int CommandTableSize, TokenTableSize;

/* Operator function pointers we need to identify tokens */
extern void op_add(void);
extern void op_subtract(void);
extern void op_mul(void);
extern void op_div(void);
extern void op_divint(void);
extern void op_mod(void);
extern void op_exp(void);
extern void op_not(void);
extern void op_inv(void);
extern void op_shiftleft(void);
extern void op_shiftright(void);
extern void op_ne(void);
extern void op_gte(void);
extern void op_lte(void);
extern void op_lt(void);
extern void op_gt(void);
extern void op_equal(void);
extern void op_and(void);
extern void op_or(void);
extern void op_xor(void);

/* Digit table from MMBasic.c for number parsing */
extern char digit[];

/* Type extraction from variable name suffix.
 * Returns T_INT for %, T_NBR for !, T_STR for $, or 0 for no suffix. */
static inline uint8_t bc_type_from_suffix(char c) {
    if (c == '%') return T_INT;
    if (c == '!') return T_NBR;
    if (c == '$') return T_STR;
    return 0;
}

/* Parse a variable name from tokenized stream, returning length.
 * Sets *type_out to the type (T_INT/T_NBR/T_STR) or 0 if untyped.
 * Sets *is_array to 1 if followed by '('. */
static inline int bc_parse_varname(unsigned char *p, uint8_t *type_out, int *is_array) {
    int len = 0;
    *type_out = 0;
    *is_array = 0;

    if (!isnamestart(*p)) return 0;
    while (isnamechar(p[len])) len++;

    /* Check for type suffix */
    if (p[len] == '%' || p[len] == '!' || p[len] == '$') {
        *type_out = bc_type_from_suffix(p[len]);
        len++;
    }

    /* Check for array */
    if (p[len] == '(') *is_array = 1;

    return len;
}

/* Parse an optional "AS INTEGER/FLOAT/STRING" type clause.
 * Returns the parsed type or 0 if no clause/type was found.
 * Advances *pp past the clause when present. */
static inline uint8_t bc_parse_as_type_clause(unsigned char **pp) {
    unsigned char *p = *pp;
    skipspace(p);
    if (*p != tokenAS) return 0;
    p++;
    skipspace(p);

    uint8_t type = 0;
    if (strncasecmp((char *)p, "INTEGER", 7) == 0 && !isnamechar(p[7])) {
        type = T_INT;
        p += 7;
    } else if (strncasecmp((char *)p, "FLOAT", 5) == 0 && !isnamechar(p[5])) {
        type = T_NBR;
        p += 5;
    } else if (strncasecmp((char *)p, "STRING", 6) == 0 && !isnamechar(p[6])) {
        type = T_STR;
        p += 6;
    }

    *pp = p;
    return type;
}

/* Compiler error helper — sets error state without aborting */
static inline void bc_set_error(BCCompiler *cs, const char *fmt, ...) {
    if (cs->has_error) return;  /* keep first error */
    cs->has_error = 1;
    cs->error_line = cs->current_line;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cs->error_msg, sizeof(cs->error_msg), fmt, ap);
    va_end(ap);
}

/* Decode a 2-byte command token from the tokenized stream */
static inline uint16_t bc_decode_cmd(const unsigned char *p) {
    return ((uint16_t)(p[0] & 0x7f)) | ((uint16_t)(p[1] & 0x7f) << 7);
}

/* Check if a byte is a command token start (both bytes >= C_BASETOKEN) */
static inline int bc_is_cmd_token(const unsigned char *p) {
    return (p[0] >= C_BASETOKEN && p[1] >= C_BASETOKEN);
}

/* Emit a LOAD for a variable (handles local vs global, scalar vs array) */
void bc_emit_load_var(BCCompiler *cs, uint16_t slot, uint8_t type, int is_local);
void bc_emit_store_var(BCCompiler *cs, uint16_t slot, uint8_t type, int is_local);

/* Expression compiler — returns the type of the compiled expression (T_INT, T_NBR, T_STR) */
uint8_t bc_compile_expression(BCCompiler *cs, unsigned char **pp);

/* Skip past a variable reference in tokenized stream (name, suffix, array indices) */
unsigned char *bc_skip_var(unsigned char *p);

/* Statement compilation */
void bc_compile_statement(BCCompiler *cs, unsigned char **pp, uint16_t cmd_token);
void bc_compile_assignment(BCCompiler *cs, unsigned char **pp);

/* Nesting stack helpers */
void bc_nest_push(BCCompiler *cs, BCNestType type);
BCNestEntry *bc_nest_top(BCCompiler *cs);
BCNestEntry *bc_nest_find(BCCompiler *cs, BCNestType type);
void bc_nest_pop(BCCompiler *cs);

/* Find a local variable in the current SUB/FUNCTION scope, returns offset or -1 */
int bc_find_local(BCCompiler *cs, const char *name, int name_len);
int bc_add_local(BCCompiler *cs, const char *name, int name_len, uint8_t type, int is_array);

/* Add a fixup for a forward reference */
void bc_add_fixup_line(BCCompiler *cs, uint32_t patch_addr, int target_line, uint8_t size, uint8_t is_relative);
void bc_resolve_fixups(BCCompiler *cs);

/* Output capture API (from bc_runtime.c) */
void bc_vm_start_capture(BCVMState *vm, char *buf, int capacity);
void bc_vm_capture_write(BCVMState *vm, const char *text, int len);
void bc_vm_capture_char(BCVMState *vm, char c);
void bc_vm_capture_string(BCVMState *vm, const char *s);

#endif /* __BC_COMPILER_INTERNAL_H */
