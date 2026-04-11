/*
 * bc_compiler_stmt.c -- Statement compiler for the bytecode VM
 *
 * Compiles individual MMBasic statements (IF, FOR, DO, WHILE, PRINT, DIM,
 * GOTO, GOSUB, SUB, FUNCTION, SELECT CASE, etc.) into bytecode.
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include "bytecode.h"
#include "bc_compiler_internal.h"

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Resolve variable: locals first, then globals.  Creates if absent. */
static uint16_t resolve_var(BCCompiler *cs, const char *name, int name_len,
                            uint8_t type, int is_array, int *is_local_out) {
    *is_local_out = 0;
    if (cs->current_subfun >= 0) {
        int loc = bc_find_local(cs, name, name_len);
        if (loc >= 0) { *is_local_out = 1; return (uint16_t)loc; }
    }
    uint16_t slot = bc_find_slot(cs, name, name_len);
    if (slot == 0xFFFF)
        slot = bc_add_slot(cs, name, name_len, type, is_array);
    return slot;
}

/* Allocate a hidden compiler-generated global slot. */
static uint16_t alloc_hidden_slot(BCCompiler *cs, uint8_t type) {
    char buf[MAXVARLEN + 1];
    snprintf(buf, sizeof(buf), "#HID_%u", (unsigned)cs->next_hidden_slot++);
    return bc_add_slot(cs, buf, (int)strlen(buf), type, 0);
}

/* Parse integer line number, returns -1 on failure. */
static int parse_line_number(unsigned char **pp) {
    unsigned char *p = *pp;
    skipspace(p);
    if (!isdigit(*p)) return -1;
    int num = 0;
    while (isdigit(*p)) { num = num * 10 + (*p - '0'); p++; }
    *pp = p;
    return num;
}

/* Emit a relative forward-jump placeholder, return patch address. */
static uint32_t emit_jmp_placeholder(BCCompiler *cs, uint8_t opcode) {
    bc_emit_byte(cs, opcode);
    uint32_t patch = cs->code_len;
    bc_emit_i16(cs, 0);
    return patch;
}

/* Patch a relative-jump placeholder to jump to cs->code_len. */
static void patch_jmp_here(BCCompiler *cs, uint32_t patch_addr) {
    bc_patch_i16(cs, patch_addr, (int16_t)(cs->code_len - (patch_addr + 2)));
}

/* Emit absolute jump/gosub to a line number (with fixup if forward ref). */
static void emit_abs_jump(BCCompiler *cs, uint8_t opcode, int lineno) {
    bc_emit_byte(cs, opcode);
    uint32_t patch = cs->code_len;
    bc_emit_u32(cs, 0);
    uint32_t target = bc_linemap_lookup(cs, (uint16_t)lineno);
    if (target != 0xFFFFFFFF)
        bc_patch_u32(cs, patch, target);
    else
        bc_add_fixup_line(cs, patch, lineno, 4, 0);
}

/*
 * Try to compile a bare SUB call: "SubName arg1, arg2, ..."
 * If *pp points to a name that is a known SUB (return_type == 0),
 * compile the call and advance *pp.  Returns 1 if compiled, 0 if not.
 */
static int try_compile_sub_call(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp;
    if (!isnamestart(*p)) return 0;

    int nlen = 0;
    while (isnamechar(p[nlen])) nlen++;

    /* Variables have type suffixes, SUBs don't */
    if (nlen > 0 && (p[nlen] == '%' || p[nlen] == '!' || p[nlen] == '$'))
        return 0;

    int sf_idx = bc_find_subfun(cs, (const char *)p, nlen);
    if (sf_idx < 0 || cs->subfuns[sf_idx].return_type != 0)
        return 0;

    /* It's a SUB call */
    p += nlen;
    skipspace(p);
    int nargs = 0;
    if (*p == '(' || (*p && *p != '\0' && *p != '\'')) {
        if (*p == '(') p++;
        while (*p && *p != ')' && *p != '\0') {
            skipspace(p);
            if (*p == ')' || *p == '\0') break;
            bc_compile_expression(cs, &p);
            nargs++;
            skipspace(p);
            if (*p == ',') p++; else break;
        }
        if (*p == ')') p++;
    }
    bc_emit_byte(cs, OP_CALL_SUB);
    bc_emit_u16(cs, (uint16_t)sf_idx);
    bc_emit_byte(cs, (uint8_t)nargs);
    *pp = p;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  IF / ELSEIF / ELSE / ENDIF                                        */
/* ------------------------------------------------------------------ */

static void compile_if(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp;
    skipspace(p);
    bc_compile_expression(cs, &p);
    skipspace(p);

    int is_single_line = 0;
    if (*p == tokenTHEN) {
        p++;
        skipspace(p);
        if (*p && *p != '\0') is_single_line = 1;
    } else if (*p == tokenGOTO) {
        is_single_line = 1;
    }

    if (is_single_line) {
        uint32_t jz_patch = emit_jmp_placeholder(cs, OP_JZ);

        if (*p == tokenGOTO) {
            p++;
            skipspace(p);
            int lineno = parse_line_number(&p);
            if (lineno < 0) { bc_set_error(cs, "Expected line number after GOTO"); *pp = p; return; }
            emit_abs_jump(cs, OP_JMP_ABS, lineno);
        } else {
            /* Compile all colon-separated statements in the THEN clause.
             * The tokenizer converts ':' to 0x00 element separators.
             * We step past 0x00 separators to compile all THEN statements.
             * Stop at T_NEWLINE, T_LINENBR, or end-of-program. */
            while (1) {
                skipspace(p);
                if (*p == 0) {
                    /* Null separator — check if more THEN-clause content follows */
                    unsigned char *peek = p + 1;
                    skipspace(peek);
                    if (*peek == 0 || *peek == T_NEWLINE || *peek == T_LINENBR) break;
                    if (*peek == tokenELSE) { p = peek; break; }
                    p = peek; /* step past null, continue with next statement */
                    continue;
                }
                if (*p == tokenELSE) break;
                if (bc_is_cmd_token(p)) {
                    uint16_t cmd = bc_decode_cmd(p); p += 2; skipspace(p);
                    bc_compile_statement(cs, &p, cmd);
                } else if (isnamestart(*p)) {
                    if (!try_compile_sub_call(cs, &p))
                        bc_compile_assignment(cs, &p);
                } else break;
            }

            if (*p == tokenELSE) {
                p++; skipspace(p);
                uint32_t jmp_patch = emit_jmp_placeholder(cs, OP_JMP);
                patch_jmp_here(cs, jz_patch);
                jz_patch = 0xFFFFFFFF;
                /* Compile all colon-separated statements in the ELSE clause */
                while (1) {
                    skipspace(p);
                    if (*p == 0) {
                        unsigned char *peek = p + 1;
                        skipspace(peek);
                        if (*peek == 0 || *peek == T_NEWLINE || *peek == T_LINENBR) break;
                        p = peek;
                        continue;
                    }
                    if (bc_is_cmd_token(p)) {
                        uint16_t cmd = bc_decode_cmd(p); p += 2; skipspace(p);
                        bc_compile_statement(cs, &p, cmd);
                    } else if (isnamestart(*p)) {
                        if (!try_compile_sub_call(cs, &p))
                            bc_compile_assignment(cs, &p);
                    } else break;
                }
                patch_jmp_here(cs, jmp_patch);
            }
        }
        if (jz_patch != 0xFFFFFFFF) patch_jmp_here(cs, jz_patch);
        *pp = p;
        return;
    }

    /* Multi-line IF */
    uint32_t jz_patch = emit_jmp_placeholder(cs, OP_JZ);
    bc_nest_push(cs, NEST_IF);
    BCNestEntry *ne = bc_nest_top(cs);
    if (ne) { ne->addr1 = jz_patch; ne->addr2 = 0xFFFFFFFF; }
    *pp = p;
}

static void compile_elseif(BCCompiler *cs, unsigned char **pp) {
    BCNestEntry *ne = bc_nest_top(cs);
    if (!ne || ne->type != NEST_IF) { bc_set_error(cs, "ELSEIF without matching IF"); return; }

    uint32_t jmp_patch = emit_jmp_placeholder(cs, OP_JMP);
    if (ne->exit_fixup_count < 16)
        ne->exit_fixups[ne->exit_fixup_count++] = jmp_patch;
    patch_jmp_here(cs, ne->addr1);

    unsigned char *p = *pp;
    skipspace(p);
    bc_compile_expression(cs, &p);
    skipspace(p);
    if (*p == tokenTHEN) p++;
    ne->addr1 = emit_jmp_placeholder(cs, OP_JZ);
    *pp = p;
}

static void compile_else(BCCompiler *cs, unsigned char **pp) {
    BCNestEntry *ne = bc_nest_top(cs);
    if (!ne || ne->type != NEST_IF) { bc_set_error(cs, "ELSE without matching IF"); return; }
    uint32_t jmp_patch = emit_jmp_placeholder(cs, OP_JMP);
    if (ne->exit_fixup_count < 16)
        ne->exit_fixups[ne->exit_fixup_count++] = jmp_patch;
    patch_jmp_here(cs, ne->addr1);
    ne->addr1 = 0xFFFFFFFF;
    ne->has_else = 1;
    (void)pp;
}

static void compile_endif(BCCompiler *cs, unsigned char **pp) {
    BCNestEntry *ne = bc_nest_top(cs);
    if (!ne || ne->type != NEST_IF) { bc_set_error(cs, "ENDIF without matching IF"); return; }
    if (ne->addr1 != 0xFFFFFFFF) patch_jmp_here(cs, ne->addr1);
    for (int i = 0; i < ne->exit_fixup_count; i++)
        patch_jmp_here(cs, ne->exit_fixups[i]);
    bc_nest_pop(cs);
    (void)pp;
}

/* ------------------------------------------------------------------ */
/*  FOR / NEXT                                                         */
/* ------------------------------------------------------------------ */

static void compile_for(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp;
    skipspace(p);

    uint8_t vtype = 0; int is_arr = 0;
    int namelen = bc_parse_varname(p, &vtype, &is_arr);
    if (namelen == 0) { bc_set_error(cs, "Expected variable after FOR"); *pp = p; return; }
    if (vtype == 0) {
        int loc = bc_find_local(cs, (const char *)p, namelen);
        if (loc >= 0) {
            vtype = cs->locals[loc].type;
        } else {
            uint16_t existing = bc_find_slot(cs, (const char *)p, namelen);
            if (existing != 0xFFFF) vtype = cs->slots[existing].type;
            else vtype = T_NBR;
        }
    }

    int is_local = 0;
    uint16_t var_slot = resolve_var(cs, (const char *)p, namelen, vtype, 0, &is_local);
    p += namelen; skipspace(p);

    if (*p == '=' || *p == tokenEQUAL) p++;
    else { bc_set_error(cs, "Expected '=' in FOR"); *pp = p; return; }
    skipspace(p);

    bc_compile_expression(cs, &p);
    bc_emit_store_var(cs, var_slot, vtype, is_local);
    skipspace(p);

    if (*p == tokenTO) p++;
    else { bc_set_error(cs, "Expected TO in FOR"); *pp = p; return; }
    skipspace(p);

    uint16_t lim_slot  = alloc_hidden_slot(cs, vtype);
    uint16_t step_slot = alloc_hidden_slot(cs, vtype);

    bc_compile_expression(cs, &p);
    bc_emit_store_var(cs, lim_slot, vtype, 0);
    skipspace(p);

    if (*p == tokenSTEP) {
        p++; skipspace(p);
        bc_compile_expression(cs, &p);
    } else {
        if (vtype == T_INT) bc_emit_byte(cs, OP_PUSH_ONE);
        else { bc_emit_byte(cs, OP_PUSH_FLT); bc_emit_f64(cs, 1.0); }
    }
    bc_emit_store_var(cs, step_slot, vtype, 0);

    /* Encode is_local flag in high bit of slot number */
    uint16_t enc_var = var_slot | (is_local ? 0x8000 : 0);

    bc_emit_byte(cs, (vtype == T_INT) ? OP_FOR_INIT_I : OP_FOR_INIT_F);
    bc_emit_u16(cs, enc_var);
    bc_emit_u16(cs, lim_slot);
    bc_emit_u16(cs, step_slot);
    uint32_t exit_patch = cs->code_len;
    bc_emit_i16(cs, 0);

    uint32_t loop_top = cs->code_len;
    bc_nest_push(cs, NEST_FOR);
    BCNestEntry *ne = bc_nest_top(cs);
    if (ne) {
        ne->addr1 = loop_top; ne->addr2 = exit_patch;
        ne->var_slot = enc_var; ne->lim_slot = lim_slot;
        ne->step_slot = step_slot; ne->var_type = vtype;
    }
    *pp = p;
}

static void compile_next(BCCompiler *cs, unsigned char **pp) {
    BCNestEntry *ne = bc_nest_find(cs, NEST_FOR);
    if (!ne) { bc_set_error(cs, "NEXT without matching FOR"); return; }

    bc_emit_byte(cs, (ne->var_type == T_INT) ? OP_FOR_NEXT_I : OP_FOR_NEXT_F);
    bc_emit_u16(cs, ne->var_slot);
    bc_emit_u16(cs, ne->lim_slot);
    bc_emit_u16(cs, ne->step_slot);
    bc_emit_i16(cs, (int16_t)(ne->addr1 - (cs->code_len + 2)));

    /* Patch FOR_INIT exit_off to jump past NEXT */
    bc_patch_i16(cs, ne->addr2, (int16_t)(cs->code_len - (ne->addr2 + 2)));

    for (int i = 0; i < ne->exit_fixup_count; i++)
        patch_jmp_here(cs, ne->exit_fixups[i]);

    /* Skip optional variable name after NEXT */
    unsigned char *p = *pp; skipspace(p);
    if (isnamestart(*p)) {
        uint8_t dt; int da;
        p += bc_parse_varname(p, &dt, &da);
    }
    *pp = p;
    bc_nest_pop(cs);
}

/* ------------------------------------------------------------------ */
/*  DO / LOOP                                                          */
/* ------------------------------------------------------------------ */

static void compile_do(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp;
    skipspace(p);

    uint32_t loop_top = cs->code_len;
    bc_nest_push(cs, NEST_DO);
    BCNestEntry *ne = bc_nest_top(cs);
    if (ne) { ne->addr1 = loop_top; ne->addr2 = 0xFFFFFFFF; }

    if (*p == tokenWHILE) {
        p++; skipspace(p);
        bc_compile_expression(cs, &p);
        if (ne) ne->addr2 = emit_jmp_placeholder(cs, OP_JZ);
    } else if (*p == tokenUNTIL) {
        p++; skipspace(p);
        bc_compile_expression(cs, &p);
        if (ne) ne->addr2 = emit_jmp_placeholder(cs, OP_JNZ);
    }
    *pp = p;
}

static void compile_loop(BCCompiler *cs, unsigned char **pp) {
    /* LOOP closes both DO...LOOP and WHILE...LOOP (PicoMite has no WEND) */
    BCNestEntry *ne = bc_nest_find(cs, NEST_DO);
    if (!ne) ne = bc_nest_find(cs, NEST_WHILE);
    if (!ne) { bc_set_error(cs, "LOOP without matching DO or WHILE"); return; }

    if (ne->type == NEST_WHILE) {
        /* WHILE...LOOP: unconditional jump back to top (condition is there) */
        bc_emit_byte(cs, OP_JMP);
        bc_emit_i16(cs, (int16_t)(ne->addr1 - (cs->code_len + 2)));
        patch_jmp_here(cs, ne->addr2);
        for (int i = 0; i < ne->exit_fixup_count; i++)
            patch_jmp_here(cs, ne->exit_fixups[i]);
        bc_nest_pop(cs);
        unsigned char *p = *pp; while (*p) p++; /* skip to end */
        *pp = p;
        return;
    }

    /* DO...LOOP with optional WHILE/UNTIL condition */
    unsigned char *p = *pp;
    skipspace(p);

    if (*p == tokenWHILE) {
        p++; skipspace(p);
        bc_compile_expression(cs, &p);
        bc_emit_byte(cs, OP_JNZ);
        bc_emit_i16(cs, (int16_t)(ne->addr1 - (cs->code_len + 2)));
    } else if (*p == tokenUNTIL) {
        p++; skipspace(p);
        bc_compile_expression(cs, &p);
        bc_emit_byte(cs, OP_JZ);
        bc_emit_i16(cs, (int16_t)(ne->addr1 - (cs->code_len + 2)));
    } else {
        bc_emit_byte(cs, OP_JMP);
        bc_emit_i16(cs, (int16_t)(ne->addr1 - (cs->code_len + 2)));
    }

    if (ne->addr2 != 0xFFFFFFFF) patch_jmp_here(cs, ne->addr2);
    for (int i = 0; i < ne->exit_fixup_count; i++)
        patch_jmp_here(cs, ne->exit_fixups[i]);
    *pp = p;
    bc_nest_pop(cs);
}

/* ------------------------------------------------------------------ */
/*  WHILE / WEND                                                       */
/* ------------------------------------------------------------------ */

static void compile_while(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp; skipspace(p);
    uint32_t loop_top = cs->code_len;
    bc_compile_expression(cs, &p);
    uint32_t jz_patch = emit_jmp_placeholder(cs, OP_JZ);
    bc_nest_push(cs, NEST_WHILE);
    BCNestEntry *ne = bc_nest_top(cs);
    if (ne) { ne->addr1 = loop_top; ne->addr2 = jz_patch; }
    *pp = p;
}

static void compile_wend(BCCompiler *cs, unsigned char **pp) {
    BCNestEntry *ne = bc_nest_find(cs, NEST_WHILE);
    if (!ne) { bc_set_error(cs, "WEND without matching WHILE"); return; }
    bc_emit_byte(cs, OP_JMP);
    bc_emit_i16(cs, (int16_t)(ne->addr1 - (cs->code_len + 2)));
    patch_jmp_here(cs, ne->addr2);
    for (int i = 0; i < ne->exit_fixup_count; i++)
        patch_jmp_here(cs, ne->exit_fixups[i]);
    bc_nest_pop(cs);
    (void)pp;
}

/* ------------------------------------------------------------------ */
/*  GOTO / GOSUB / RETURN                                              */
/* ------------------------------------------------------------------ */

static void compile_goto(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp; skipspace(p);
    int lineno = parse_line_number(&p);
    if (lineno < 0) { bc_set_error(cs, "Expected line number after GOTO"); *pp = p; return; }
    emit_abs_jump(cs, OP_JMP_ABS, lineno);
    *pp = p;
}

static void compile_gosub(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp; skipspace(p);
    int lineno = parse_line_number(&p);
    if (lineno < 0) { bc_set_error(cs, "Expected line number after GOSUB"); *pp = p; return; }
    emit_abs_jump(cs, OP_GOSUB, lineno);
    *pp = p;
}

/* ------------------------------------------------------------------ */
/*  PRINT                                                              */
/* ------------------------------------------------------------------ */

static void compile_print(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp; skipspace(p);
    int suppress_newline = 0;

    while (*p && *p != '\'' && *p != T_NEWLINE && *p != tokenELSE) {
        skipspace(p);
        if (*p == '\0' || *p == '\'' || *p == T_NEWLINE || *p == tokenELSE) break;
        if (*p == ';') { suppress_newline = 1; p++; continue; }
        if (*p == ',') { bc_emit_byte(cs, OP_PRINT_TAB); suppress_newline = 1; p++; continue; }

        suppress_newline = 0;
        uint8_t etype = bc_compile_expression(cs, &p);
        uint8_t flags = PRINT_NO_NEWLINE;
        uint8_t op;
        switch (etype & (T_INT | T_NBR | T_STR)) {
            case T_INT: op = OP_PRINT_INT; break;
            case T_NBR: op = OP_PRINT_FLT; break;
            case T_STR: op = OP_PRINT_STR; break;
            default:    op = OP_PRINT_INT; break;
        }
        bc_emit_byte(cs, op);
        bc_emit_byte(cs, flags);
        skipspace(p);
    }
    if (!suppress_newline) bc_emit_byte(cs, OP_PRINT_NEWLINE);
    *pp = p;
}

/* ------------------------------------------------------------------ */
/*  DIM                                                                */
/* ------------------------------------------------------------------ */

static void compile_dim(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp;

    /* Check for DIM INTEGER / DIM FLOAT / DIM STRING — sets default type
     * for all variables in this DIM statement */
    uint8_t forced_type = 0;
    skipspace(p);
    if (strncasecmp((char *)p, "INTEGER", 7) == 0 && !isnamechar(p[7])) {
        forced_type = T_INT; p += 7;
    } else if (strncasecmp((char *)p, "FLOAT", 5) == 0 && !isnamechar(p[5])) {
        forced_type = T_NBR; p += 5;
    } else if (strncasecmp((char *)p, "STRING", 6) == 0 && !isnamechar(p[6])) {
        forced_type = T_STR; p += 6;
    }

    while (1) {
        skipspace(p);
        if (!isnamestart(*p)) break;
        uint8_t vtype = 0; int is_arr = 0;
        int namelen = bc_parse_varname(p, &vtype, &is_arr);
        if (namelen == 0) break;
        if (vtype == 0) vtype = forced_type ? forced_type : T_NBR;

        uint16_t slot = bc_find_slot(cs, (const char *)p, namelen);
        if (slot == 0xFFFF) slot = bc_add_slot(cs, (const char *)p, namelen, vtype, is_arr);
        p += namelen; skipspace(p);

        if (*p == '(') {
            /* Array DIM: DIM arr(size) */
            p++;
            int ndim = 0;
            while (1) {
                skipspace(p);
                bc_compile_expression(cs, &p);
                ndim++;
                skipspace(p);
                if (*p == ',') p++; else break;
            }
            if (*p == ')') p++;
            skipspace(p);
            if (*p == tokenAS) { p++; skipspace(p); while (isnamechar(*p)) p++; }

            uint8_t dim_op;
            switch (vtype & (T_INT | T_NBR | T_STR)) {
                case T_INT: dim_op = OP_DIM_ARR_I; break;
                case T_STR: dim_op = OP_DIM_ARR_S; break;
                default:    dim_op = OP_DIM_ARR_F; break;
            }
            bc_emit_byte(cs, dim_op);
            bc_emit_u16(cs, slot);
            bc_emit_byte(cs, (uint8_t)ndim);
        } else if (*p == '=' || *p == tokenEQUAL) {
            /* Scalar DIM with init: DIM a% = expr */
            p++; skipspace(p);
            int is_local = (cs->current_subfun >= 0) ? 1 : 0;
            bc_compile_expression(cs, &p);
            bc_emit_store_var(cs, slot, vtype, is_local);
        } else if (*p == tokenAS) {
            /* DIM varname AS type — skip the AS clause */
            p++; skipspace(p); while (isnamechar(*p)) p++;
        }
        /* else: bare DIM varname — just declaring, slot already created */

        skipspace(p);
        if (*p == ',') p++; else break;
    }
    *pp = p;
}

/* ------------------------------------------------------------------ */
/*  SUB / END SUB                                                      */
/* ------------------------------------------------------------------ */

/* Parse parameter list: (p1, p2, ...) and register as locals */
static int parse_params(BCCompiler *cs, unsigned char **pp, int sf_idx) {
    unsigned char *p = *pp;
    int nparams = 0;
    if (*p != '(') { *pp = p; return 0; }
    p++;
    while (1) {
        skipspace(p);
        if (*p == ')' || *p == '\0') break;
        uint8_t ptype = 0; int pa = 0;
        unsigned char *name_start = p;
        int plen = bc_parse_varname(p, &ptype, &pa);
        if (plen == 0) break;
        p += plen;
        if (pa && *p == '(') {
            int depth = 0;
            do { if (*p == '(') depth++; else if (*p == ')') depth--;
                 else if (*p == '\0') break; p++; } while (depth > 0);
        }
        uint8_t as_type = bc_parse_as_type_clause(&p);
        if (as_type != 0) ptype = as_type;
        if (ptype == 0) ptype = T_NBR;
        bc_add_local(cs, (const char *)name_start, plen, ptype, pa);
        cs->subfuns[sf_idx].param_types[nparams] = ptype;
        cs->subfuns[sf_idx].param_is_array[nparams] = (uint8_t)pa;
        nparams++;
        skipspace(p);
        if (*p == ',') p++;
    }
    if (*p == ')') p++;
    *pp = p;
    return nparams;
}

/* Find or create a subfun entry. */
static int get_or_create_subfun(BCCompiler *cs, const char *name, int namelen, uint8_t ret_type) {
    int idx = bc_find_subfun(cs, name, namelen);
    if (idx >= 0) { cs->subfuns[idx].return_type = ret_type; return idx; }
    if (cs->subfun_count >= BC_MAX_SUBFUNS) {
        bc_set_error(cs, "Too many SUB/FUNCTION definitions");
        return -1;
    }
    idx = cs->subfun_count++;
    int clen = (namelen > MAXVARLEN) ? MAXVARLEN : namelen;
    memcpy(cs->subfuns[idx].name, name, clen);
    cs->subfuns[idx].name[clen] = '\0';
    cs->subfuns[idx].return_type = ret_type;
    return idx;
}

static void compile_sub(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp; skipspace(p);
    int namelen = 0;
    while (isnamechar(p[namelen])) namelen++;
    if (namelen == 0) { bc_set_error(cs, "Expected SUB name"); *pp = p; return; }

    int sf_idx = get_or_create_subfun(cs, (const char *)p, namelen, 0);
    if (sf_idx < 0) { *pp = p; return; }
    p += namelen; skipspace(p);

    uint32_t skip_patch = emit_jmp_placeholder(cs, OP_JMP);
    cs->subfuns[sf_idx].entry_addr = cs->code_len;
    cs->current_subfun = sf_idx;
    cs->local_count = 0;

    cs->subfuns[sf_idx].nparams = (uint8_t)parse_params(cs, &p, sf_idx);

    bc_emit_byte(cs, OP_ENTER_FRAME);
    uint32_t nlocals_patch = cs->code_len;
    bc_emit_u16(cs, 0);

    bc_nest_push(cs, NEST_SUB);
    BCNestEntry *ne = bc_nest_top(cs);
    if (ne) { ne->addr1 = skip_patch; ne->addr2 = nlocals_patch; }
    *pp = p;
}

static void compile_end_sub(BCCompiler *cs, unsigned char **pp) {
    BCNestEntry *ne = bc_nest_top(cs);
    if (!ne || ne->type != NEST_SUB) { bc_set_error(cs, "END SUB without matching SUB"); return; }
    bc_patch_u16(cs, ne->addr2, cs->local_count);
    if (cs->current_subfun >= 0) {
        cs->subfuns[cs->current_subfun].nlocals = cs->local_count;
        bc_commit_locals(cs, cs->current_subfun);
    }
    bc_emit_byte(cs, OP_LEAVE_FRAME);
    bc_emit_byte(cs, OP_RET_SUB);
    patch_jmp_here(cs, ne->addr1);
    cs->current_subfun = -1; cs->local_count = 0;
    bc_nest_pop(cs);
    (void)pp;
}

/* ------------------------------------------------------------------ */
/*  FUNCTION / END FUNCTION                                            */
/* ------------------------------------------------------------------ */

static void compile_function(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp; skipspace(p);
    uint8_t ret_type = 0; int is_arr = 0;
    unsigned char *name_start = p;  /* save pointer to full name including suffix */
    int namelen = bc_parse_varname(p, &ret_type, &is_arr);
    if (namelen == 0) { bc_set_error(cs, "Expected FUNCTION name"); *pp = p; return; }
    int has_suffix = (ret_type != 0);

    /* Subfun names are stored WITHOUT type suffix */
    int sf_namelen = has_suffix ? namelen - 1 : namelen;

    /* If no suffix on name, check for AS type after the param list.
     * Pass 1 already parsed this, so use the stored return_type. */
    if (ret_type == 0) {
        int sf_idx_tmp = bc_find_subfun(cs, (const char *)p, sf_namelen);
        if (sf_idx_tmp >= 0 && cs->subfuns[sf_idx_tmp].return_type != 0)
            ret_type = cs->subfuns[sf_idx_tmp].return_type;
        else
            ret_type = T_NBR;  /* default to float */
    }

    int sf_idx = get_or_create_subfun(cs, (const char *)p, sf_namelen, ret_type);
    if (sf_idx < 0) { *pp = p; return; }
    p += namelen; skipspace(p);

    uint32_t skip_patch = emit_jmp_placeholder(cs, OP_JMP);
    cs->subfuns[sf_idx].entry_addr = cs->code_len;
    cs->current_subfun = sf_idx;
    cs->local_count = 0;

    /* Function name is local slot 0 (return value).
       Use the base name (without suffix) for unsuffixed functions so that
       assignments like "Double = x%" can find it via bc_find_local. */
    bc_add_local(cs, (const char *)name_start, has_suffix ? namelen : sf_namelen, ret_type, 0);

    cs->subfuns[sf_idx].nparams = (uint8_t)parse_params(cs, &p, sf_idx);
    (void)bc_parse_as_type_clause(&p);

    bc_emit_byte(cs, OP_ENTER_FRAME);
    uint32_t nlocals_patch = cs->code_len;
    bc_emit_u16(cs, 0);

    bc_nest_push(cs, NEST_FUNCTION);
    BCNestEntry *ne = bc_nest_top(cs);
    if (ne) { ne->addr1 = skip_patch; ne->addr2 = nlocals_patch;
              ne->var_slot = 0; ne->var_type = ret_type; }
    *pp = p;
}

static void compile_end_function(BCCompiler *cs, unsigned char **pp) {
    BCNestEntry *ne = bc_nest_top(cs);
    if (!ne || ne->type != NEST_FUNCTION) {
        bc_set_error(cs, "END FUNCTION without matching FUNCTION"); return;
    }
    bc_patch_u16(cs, ne->addr2, cs->local_count);
    if (cs->current_subfun >= 0) {
        cs->subfuns[cs->current_subfun].nlocals = cs->local_count;
        bc_commit_locals(cs, cs->current_subfun);
    }
    bc_emit_load_var(cs, ne->var_slot, ne->var_type, 1);
    bc_emit_byte(cs, OP_LEAVE_FRAME);
    bc_emit_byte(cs, OP_RET_FUN);
    patch_jmp_here(cs, ne->addr1);
    cs->current_subfun = -1; cs->local_count = 0;
    bc_nest_pop(cs);
    (void)pp;
}

/* ------------------------------------------------------------------ */
/*  LOCAL                                                              */
/* ------------------------------------------------------------------ */

static void compile_local(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp;
    if (cs->current_subfun < 0) { bc_set_error(cs, "LOCAL outside SUB/FUNCTION"); *pp = p; return; }

    /* Check for LOCAL INTEGER / LOCAL FLOAT / LOCAL STRING */
    uint8_t forced_type = 0;
    skipspace(p);
    if (strncasecmp((char *)p, "INTEGER", 7) == 0 && !isnamechar(p[7])) {
        forced_type = T_INT; p += 7;
    } else if (strncasecmp((char *)p, "FLOAT", 5) == 0 && !isnamechar(p[5])) {
        forced_type = T_NBR; p += 5;
    } else if (strncasecmp((char *)p, "STRING", 6) == 0 && !isnamechar(p[6])) {
        forced_type = T_STR; p += 6;
    }

    while (1) {
        skipspace(p);
        if (!isnamestart(*p)) break;
        unsigned char *name_start = p;
        uint8_t vtype = 0; int is_arr = 0;
        int namelen = bc_parse_varname(p, &vtype, &is_arr);
        if (namelen == 0) break;
        p += namelen;
        if (is_arr && *p == '(') {
            int depth = 0;
            do { if (*p == '(') depth++; else if (*p == ')') depth--;
                 else if (*p == '\0') break; p++; } while (depth > 0);
        }
        uint8_t as_type = bc_parse_as_type_clause(&p);
        if (as_type != 0) vtype = as_type;
        if (vtype == 0) vtype = forced_type ? forced_type : T_NBR;
        bc_add_local(cs, (const char *)name_start, namelen, vtype, is_arr);
        skipspace(p);
        if (*p == ',') p++; else break;
    }
    *pp = p;
}

/* ------------------------------------------------------------------ */
/*  SELECT CASE / CASE / CASE ELSE / END SELECT                       */
/* ------------------------------------------------------------------ */

static void compile_select_case(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp; skipspace(p);
    uint8_t sel_type = bc_compile_expression(cs, &p);
    uint16_t sel_slot = alloc_hidden_slot(cs, sel_type);
    bc_emit_store_var(cs, sel_slot, sel_type, 0);
    bc_nest_push(cs, NEST_SELECT);
    BCNestEntry *ne = bc_nest_top(cs);
    if (ne) { ne->select_slot = sel_slot; ne->select_type = sel_type; ne->addr1 = 0xFFFFFFFF; }
    *pp = p;
}

static void compile_case(BCCompiler *cs, unsigned char **pp) {
    BCNestEntry *ne = bc_nest_find(cs, NEST_SELECT);
    if (!ne) { bc_set_error(cs, "CASE without matching SELECT CASE"); return; }

    /* Emit JMP to END SELECT at end of previous case body (skip if first CASE) */
    if (ne->addr1 != 0xFFFFFFFF) {
        uint32_t end_jmp = emit_jmp_placeholder(cs, OP_JMP);
        if (ne->case_end_count < 16) ne->case_end_fixups[ne->case_end_count++] = end_jmp;
        patch_jmp_here(cs, ne->addr1);
    }

    unsigned char *p = *pp; skipspace(p);
    int first = 1;
    while (1) {
        skipspace(p);
        if (*p == '\0') break;
        bc_emit_load_var(cs, ne->select_slot, ne->select_type, 0);
        bc_compile_expression(cs, &p);
        switch (ne->select_type & (T_INT | T_NBR | T_STR)) {
            case T_INT: bc_emit_byte(cs, OP_EQ_I); break;
            case T_NBR: bc_emit_byte(cs, OP_EQ_F); break;
            case T_STR: bc_emit_byte(cs, OP_EQ_S); break;
            default:    bc_emit_byte(cs, OP_EQ_I); break;
        }
        if (!first) bc_emit_byte(cs, OP_OR);
        first = 0;
        skipspace(p);
        if (*p == ',') p++; else break;
    }
    ne->addr1 = emit_jmp_placeholder(cs, OP_JZ);
    *pp = p;
}

static void compile_case_else(BCCompiler *cs, unsigned char **pp) {
    BCNestEntry *ne = bc_nest_find(cs, NEST_SELECT);
    if (!ne) { bc_set_error(cs, "CASE ELSE without SELECT CASE"); return; }

    /* Emit JMP to END SELECT at end of previous case body */
    if (ne->addr1 != 0xFFFFFFFF) {
        uint32_t end_jmp = emit_jmp_placeholder(cs, OP_JMP);
        if (ne->case_end_count < 16) ne->case_end_fixups[ne->case_end_count++] = end_jmp;
        patch_jmp_here(cs, ne->addr1);
    }
    ne->addr1 = 0xFFFFFFFF;
    (void)pp;
}

static void compile_end_select(BCCompiler *cs, unsigned char **pp) {
    BCNestEntry *ne = bc_nest_find(cs, NEST_SELECT);
    if (!ne) { bc_set_error(cs, "END SELECT without SELECT CASE"); return; }
    if (ne->addr1 != 0xFFFFFFFF) patch_jmp_here(cs, ne->addr1);
    for (int i = 0; i < ne->case_end_count; i++)
        patch_jmp_here(cs, ne->case_end_fixups[i]);
    bc_nest_pop(cs);
    (void)pp;
}

/* ------------------------------------------------------------------ */
/*  EXIT FOR / EXIT DO                                                 */
/* ------------------------------------------------------------------ */

static void compile_exit_for(BCCompiler *cs, unsigned char **pp) {
    BCNestEntry *ne = bc_nest_find(cs, NEST_FOR);
    if (!ne) { bc_set_error(cs, "EXIT FOR without matching FOR"); return; }
    uint32_t patch = emit_jmp_placeholder(cs, OP_JMP);
    if (ne->exit_fixup_count < 16) ne->exit_fixups[ne->exit_fixup_count++] = patch;
}

static void compile_exit_do(BCCompiler *cs, unsigned char **pp) {
    BCNestEntry *ne = bc_nest_find(cs, NEST_DO);
    if (!ne) { bc_set_error(cs, "EXIT DO without matching DO"); return; }
    uint32_t patch = emit_jmp_placeholder(cs, OP_JMP);
    if (ne->exit_fixup_count < 16) ne->exit_fixups[ne->exit_fixup_count++] = patch;
}

static void compile_exit(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp; skipspace(p);
    BCNestType target_type;
    int skip_len = 0;

    if (*p == tokenFOR) {
        target_type = NEST_FOR; skip_len = 1;
    } else if (isnamestart(*p) && strncasecmp((char *)p, "FOR", 3) == 0 && !isnamechar(p[3])) {
        target_type = NEST_FOR; skip_len = 3;
    } else if (strncasecmp((char *)p, "DO", 2) == 0 && !isnamechar(p[2])) {
        target_type = NEST_DO; skip_len = 2;
    } else if (strncasecmp((char *)p, "FUNCTION", 8) == 0 && !isnamechar(p[8])) {
        /* EXIT FUNCTION — load return value, leave frame, return */
        BCNestEntry *ne = bc_nest_find(cs, NEST_FUNCTION);
        if (!ne) { bc_set_error(cs, "EXIT FUNCTION without matching FUNCTION"); *pp = p; return; }
        bc_emit_load_var(cs, ne->var_slot, ne->var_type, 1);
        bc_emit_byte(cs, OP_LEAVE_FRAME);
        bc_emit_byte(cs, OP_RET_FUN);
        p += 8; *pp = p; return;
    } else if (strncasecmp((char *)p, "SUB", 3) == 0 && !isnamechar(p[3])) {
        /* EXIT SUB — leave frame, return */
        BCNestEntry *ne = bc_nest_find(cs, NEST_SUB);
        if (!ne) { bc_set_error(cs, "EXIT SUB without matching SUB"); *pp = p; return; }
        bc_emit_byte(cs, OP_LEAVE_FRAME);
        bc_emit_byte(cs, OP_RET_SUB);
        p += 3; *pp = p; return;
    } else {
        bc_set_error(cs, "Expected FOR, DO, FUNCTION or SUB after EXIT"); *pp = p; return;
    }

    BCNestEntry *ne = bc_nest_find(cs, target_type);
    if (!ne) {
        bc_set_error(cs, "EXIT without matching loop"); *pp = p; return;
    }
    uint32_t patch = emit_jmp_placeholder(cs, OP_JMP);
    if (ne->exit_fixup_count < 16) ne->exit_fixups[ne->exit_fixup_count++] = patch;
    p += skip_len;
    *pp = p;
}

static void compile_exit_function(BCCompiler *cs, unsigned char **pp) {
    BCNestEntry *ne = bc_nest_find(cs, NEST_FUNCTION);
    if (!ne) { bc_set_error(cs, "EXIT FUNCTION without matching FUNCTION"); return; }
    bc_emit_load_var(cs, ne->var_slot, ne->var_type, 1);
    bc_emit_byte(cs, OP_LEAVE_FRAME);
    bc_emit_byte(cs, OP_RET_FUN);
    (void)pp;
}

static void compile_exit_sub(BCCompiler *cs, unsigned char **pp) {
    BCNestEntry *ne = bc_nest_find(cs, NEST_SUB);
    if (!ne) { bc_set_error(cs, "EXIT SUB without matching SUB"); return; }
    bc_emit_byte(cs, OP_LEAVE_FRAME);
    bc_emit_byte(cs, OP_RET_SUB);
    (void)pp;
}

/* ------------------------------------------------------------------ */
/*  DATA: parse values into the data pool at compile time              */
/* ------------------------------------------------------------------ */

static void compile_data(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp;
    skipspace(p);

    /* Cache operator tokens for +/- which get tokenized in DATA lines */
    static unsigned char tokMinus = 0, tokPlus = 0;
    if (!tokMinus) {
        tokMinus = (unsigned char)GetTokenValue((unsigned char *)"-");
        tokPlus  = (unsigned char)GetTokenValue((unsigned char *)"+");
    }

    while (*p && *p != '\'' && *p != T_NEWLINE && *p != tokenELSE) {
        skipspace(p);
        if (!*p || *p == '\'' || *p == T_NEWLINE || *p == tokenELSE) break;

        if (cs->data_count >= BC_MAX_DATA_ITEMS) {
            bc_set_error(cs, "Too many DATA items");
            return;
        }

        BCDataItem *item = &cs->data_pool[cs->data_count];

        if (*p == '"') {
            /* Quoted string — copy contents between quotes */
            p++;  /* skip opening quote */
            uint8_t strbuf[STRINGSIZE];
            int len = 0;
            while (*p && *p != '"' && len < STRINGSIZE - 1) {
                strbuf[len++] = *p++;
            }
            if (*p == '"') p++;  /* skip closing quote */

            /* Store as string constant pool index */
            uint16_t cidx = bc_add_constant_string(cs, strbuf, len);
            item->value.i = cidx;  /* store constant pool index */
            item->type = T_STR;
            cs->data_count++;
        } else if (isdigit(*p) || *p == '.' || *p == tokMinus || *p == '-' ||
                   *p == tokPlus || *p == '+') {
            /* Numeric value — parse as number */
            int neg = 0;

            /* Handle tokenized or raw +/- signs */
            if (*p == tokMinus || *p == '-') { neg = 1; p++; skipspace(p); }
            else if (*p == tokPlus || *p == '+') { p++; skipspace(p); }

            /* Check if it's a float (has decimal point or exponent) */
            int is_float = 0;
            unsigned char *scan = p;
            while (isdigit(*scan)) scan++;
            if (*scan == '.') is_float = 1;
            if (*scan == 'e' || *scan == 'E') is_float = 1;

            if (is_float) {
                /* Parse as float */
                char numbuf[64];
                int nlen = 0;
                if (neg && nlen < 63) numbuf[nlen++] = '-';
                while ((*p >= '0' && *p <= '9') || *p == '.' ||
                       *p == 'e' || *p == 'E' || *p == '+' || *p == '-') {
                    if (nlen < 63) numbuf[nlen++] = *p;
                    p++;
                }
                numbuf[nlen] = '\0';
                item->value.f = strtod(numbuf, NULL);
                item->type = T_NBR;
            } else {
                /* Parse as integer */
                int64_t val = 0;
                while (isdigit(*p)) {
                    val = val * 10 + (*p - '0');
                    p++;
                }
                if (neg) val = -val;
                item->value.i = val;
                item->type = T_INT;
            }
            cs->data_count++;
        } else {
            /* Unquoted string — read until comma or end of line */
            uint8_t strbuf[STRINGSIZE];
            int len = 0;
            while (*p && *p != ',' && *p != '\'' && len < STRINGSIZE - 1) {
                strbuf[len++] = *p++;
            }
            /* Trim trailing spaces */
            while (len > 0 && strbuf[len - 1] == ' ') len--;
            uint16_t cidx = bc_add_constant_string(cs, strbuf, len);
            item->value.i = cidx;
            item->type = T_STR;
            cs->data_count++;
        }

        skipspace(p);
        if (*p == ',') { p++; continue; }
        break;
    }
    /* Skip rest of line */
    while (*p) p++;
    *pp = p;
}

/* ------------------------------------------------------------------ */
/*  READ: emit opcodes to read from the data pool                      */
/* ------------------------------------------------------------------ */

static void compile_read(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp;
    skipspace(p);

    while (*p && *p != '\'' && *p != T_NEWLINE && *p != tokenELSE) {
        skipspace(p);
        if (!*p || *p == '\'' || *p == T_NEWLINE || *p == tokenELSE) break;

        /* Parse variable name */
        uint8_t vtype = 0;
        int is_arr = 0;
        int namelen = bc_parse_varname(p, &vtype, &is_arr);
        if (namelen == 0) {
            bc_set_error(cs, "Expected variable in READ");
            return;
        }
        if (vtype == 0) vtype = T_NBR;

        int is_local = 0;
        uint16_t slot = resolve_var(cs, (const char *)p, namelen, vtype, is_arr, &is_local);
        p += namelen;

        if (is_arr && *p == '(') {
            /* READ into array element */
            p++;
            int ndim = 0;
            while (1) {
                skipspace(p);
                bc_compile_expression(cs, &p);
                ndim++;
                skipspace(p);
                if (*p == ',') p++; else break;
            }
            if (*p == ')') p++;

            /* Emit READ to push value, then STORE_ARR */
            switch (vtype & (T_INT | T_NBR | T_STR)) {
                case T_INT: bc_emit_byte(cs, OP_READ_I); break;
                case T_STR: bc_emit_byte(cs, OP_READ_S); break;
                default:    bc_emit_byte(cs, OP_READ_F); break;
            }
            if (is_local) {
                switch (vtype & (T_INT | T_NBR | T_STR)) {
                    case T_INT: bc_emit_byte(cs, OP_STORE_LOCAL_ARR_I); break;
                    case T_STR: bc_emit_byte(cs, OP_STORE_LOCAL_ARR_S); break;
                    default:    bc_emit_byte(cs, OP_STORE_LOCAL_ARR_F); break;
                }
            } else {
                switch (vtype & (T_INT | T_NBR | T_STR)) {
                    case T_INT: bc_emit_byte(cs, OP_STORE_ARR_I); break;
                    case T_STR: bc_emit_byte(cs, OP_STORE_ARR_S); break;
                    default:    bc_emit_byte(cs, OP_STORE_ARR_F); break;
                }
            }
            bc_emit_u16(cs, slot);
            bc_emit_byte(cs, (uint8_t)ndim);
        } else {
            /* READ into scalar */
            switch (vtype & (T_INT | T_NBR | T_STR)) {
                case T_INT: bc_emit_byte(cs, OP_READ_I); break;
                case T_STR: bc_emit_byte(cs, OP_READ_S); break;
                default:    bc_emit_byte(cs, OP_READ_F); break;
            }
            bc_emit_store_var(cs, slot, vtype, is_local);
        }

        skipspace(p);
        if (*p == ',') { p++; continue; }
        break;
    }
    *pp = p;
}

/* ------------------------------------------------------------------ */
/*  RESTORE: reset data pointer                                        */
/* ------------------------------------------------------------------ */

static void compile_restore(BCCompiler *cs, unsigned char **pp) {
    bc_emit_byte(cs, OP_RESTORE);
    unsigned char *p = *pp;
    while (*p) p++;
    *pp = p;
}

/* ------------------------------------------------------------------ */
/*  INC var [, amount]                                                 */
/* ------------------------------------------------------------------ */

static void compile_inc(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp;
    skipspace(p);

    uint8_t vtype = 0; int is_arr = 0;
    int namelen = bc_parse_varname(p, &vtype, &is_arr);
    if (namelen == 0) { bc_set_error(cs, "Expected variable in INC"); return; }
    if (vtype == 0) vtype = T_NBR;
    if (vtype & T_STR) {
        /* String INC (concatenation) is not native yet. */
        *pp = p; return;  /* caller will report unsupported command */
    }

    int is_local = 0;
    uint16_t slot = resolve_var(cs, (const char *)p, namelen, vtype, is_arr, &is_local);
    p += namelen; skipspace(p);

    /* Load current value */
    bc_emit_load_var(cs, slot, vtype, is_local);

    if (*p == ',') {
        /* INC var, amount — compile the amount expression */
        p++; skipspace(p);
        bc_compile_expression(cs, &p);
    } else {
        /* INC var — increment by 1 */
        bc_emit_byte(cs, OP_PUSH_ONE);
        if (vtype == T_NBR) bc_emit_byte(cs, OP_CVT_I2F);
    }

    /* Add and store back */
    if (vtype == T_INT)
        bc_emit_byte(cs, OP_ADD_I);
    else
        bc_emit_byte(cs, OP_ADD_F);
    bc_emit_store_var(cs, slot, vtype, is_local);

    while (*p) p++;
    *pp = p;
}

/* ------------------------------------------------------------------ */
/*  CONST name = expr [, name2 = expr2, ...]                           */
/* ------------------------------------------------------------------ */

static void compile_const(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp;

    while (1) {
        skipspace(p);
        if (!isnamestart(*p)) break;

        uint8_t vtype = 0; int is_arr = 0;
        int namelen = bc_parse_varname(p, &vtype, &is_arr);
        if (namelen == 0) break;
        if (vtype == 0) vtype = T_NBR;

        int is_local = 0;
        uint16_t slot = resolve_var(cs, (const char *)p, namelen, vtype, is_arr, &is_local);
        p += namelen; skipspace(p);

        if (*p == '=' || *p == tokenEQUAL) {
            p++; skipspace(p);
            bc_compile_expression(cs, &p);
            bc_emit_store_var(cs, slot, vtype, is_local);
        } else {
            bc_set_error(cs, "Expected = in CONST"); return;
        }
        skipspace(p);
        if (*p == ',') p++; else break;
    }
    *pp = p;
}

/* ------------------------------------------------------------------ */
/*  RANDOMIZE [seed]                                                   */
/* ------------------------------------------------------------------ */

static void compile_randomize(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp;
    skipspace(p);
    if (*p && *p != '\'' && *p != 0) {
        /* RANDOMIZE seed — compile the expression */
        bc_compile_expression(cs, &p);
        bc_emit_byte(cs, OP_RANDOMIZE);
    } else {
        /* RANDOMIZE with no arg — push 0 as sentinel for "use time" */
        bc_emit_byte(cs, OP_PUSH_ZERO);
        bc_emit_byte(cs, OP_RANDOMIZE);
    }
    while (*p) p++;
    *pp = p;
}

/* ------------------------------------------------------------------ */
/*  ERROR ["message"]                                                  */
/* ------------------------------------------------------------------ */

static void compile_error(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp;
    skipspace(p);
    if (*p && *p != '\'' && *p != 0) {
        bc_compile_expression(cs, &p);
        bc_emit_byte(cs, OP_ERROR_S);
    } else {
        bc_emit_byte(cs, OP_ERROR_EMPTY);
    }
    while (*p) p++;
    *pp = p;
}

/* ------------------------------------------------------------------ */
/*  CLEAR                                                              */
/* ------------------------------------------------------------------ */

static void compile_clear(BCCompiler *cs, unsigned char **pp) {
    if (cs->current_subfun >= 0) {
        bc_set_error(cs, "CLEAR invalid in a subroutine");
        return;
    }
    bc_emit_byte(cs, OP_CLEAR);
    unsigned char *p = *pp;
    while (*p) p++;
    *pp = p;
}

static int cmdline_matches_exact(unsigned char *p, const char *kw) {
    unsigned short want = 0;
    unsigned char *rest;

    skipspace(p);
    if (bc_is_cmd_token(p)) {
        if (strcmp(kw, "SWAP") == 0) {
            want = (unsigned short)GetCommandValue((unsigned char *)"Swap");
        } else if (strcmp(kw, "SYNC") == 0) {
            want = (unsigned short)GetCommandValue((unsigned char *)"Sync");
        } else if (strcmp(kw, "CREATE") == 0) {
            want = (unsigned short)GetCommandValue((unsigned char *)"Create");
        } else if (strcmp(kw, "CLOSE") == 0) {
            want = (unsigned short)GetCommandValue((unsigned char *)"Close");
        } else if (strcmp(kw, "FPS") == 0) {
            want = (unsigned short)GetCommandValue((unsigned char *)"FPS");
        }
        if (want && bc_decode_cmd(p) == want) {
            p += 2;
            skipspace(p);
            return *p == 0;
        }
    }

    rest = checkstring(p, (unsigned char *)kw);
    if (!rest) return 0;
    skipspace(rest);
    return *rest == 0;
}

static unsigned char *cmdline_match_prefix(unsigned char *p, const char *kw) {
    unsigned short want = 0;
    unsigned char *rest;

    skipspace(p);
    if (bc_is_cmd_token(p)) {
        if (strcmp(kw, "SWAP") == 0) {
            want = (unsigned short)GetCommandValue((unsigned char *)"Swap");
        } else if (strcmp(kw, "SYNC") == 0) {
            want = (unsigned short)GetCommandValue((unsigned char *)"Sync");
        } else if (strcmp(kw, "CREATE") == 0) {
            want = (unsigned short)GetCommandValue((unsigned char *)"Create");
        } else if (strcmp(kw, "CLOSE") == 0) {
            want = (unsigned short)GetCommandValue((unsigned char *)"Close");
        } else if (strcmp(kw, "FPS") == 0) {
            want = (unsigned short)GetCommandValue((unsigned char *)"FPS");
        }
        if (want && bc_decode_cmd(p) == want) {
            p += 2;
            skipspace(p);
            return p;
        }
    }

    rest = checkstring(p, (unsigned char *)kw);
    if (!rest) return NULL;
    skipspace(rest);
    return rest;
}

typedef struct {
    unsigned char *start;
    unsigned char *end;
    uint8_t kind;
    uint16_t slot;
    uint8_t type;
    uint8_t present;
} BCBoxArgSpec;

static unsigned char *skipspace_to(unsigned char *p, unsigned char *end) {
    while (p < end && *p == ' ') p++;
    return p;
}

static unsigned char *trim_end_space(unsigned char *start, unsigned char *end) {
    while (end > start && end[-1] == ' ') end--;
    return end;
}

static int parse_box_args(unsigned char *p, BCBoxArgSpec *args, int max_fields,
                          int *field_count_out, unsigned char **end_out) {
    int field = 0;

    while (1) {
        unsigned char *start = p;
        unsigned char *end = p;
        int depth = 0;
        int in_string = 0;

        while (*end) {
            if (*end == '"') {
                in_string = !in_string;
            } else if (!in_string) {
                if (*end == '(') depth++;
                else if (*end == ')' && depth > 0) depth--;
                else if (*end == ',' && depth == 0) break;
            }
            end++;
        }

        if (field >= max_fields) return -1;

        args[field].start = skipspace_to(start, end);
        args[field].end = trim_end_space(args[field].start, end);
        args[field].present = (args[field].start < args[field].end);
        args[field].kind = BC_BOX_ARG_EMPTY;
        args[field].slot = 0;
        args[field].type = 0;
        field++;

        if (*end != ',') {
            *field_count_out = field;
            *end_out = end;
            return 0;
        }
        p = end + 1;
    }
}

static int try_parse_box_array_ref(BCCompiler *cs, BCBoxArgSpec *arg, const char *cmd_name) {
    unsigned char tmp[STRINGSIZE];
    unsigned char *p = tmp;
    unsigned char *end;
    uint8_t type_hint = 0;
    int is_array_hint = 0;
    int seg_len;
    int name_len;
    int is_local = 0;
    int loc;
    uint16_t slot;
    uint8_t type = 0;
    int symbol_is_array = 0;

    if (!arg->present || !isnamestart(*arg->start)) return 0;

    seg_len = (int)(arg->end - arg->start);
    if (seg_len <= 0 || seg_len >= STRINGSIZE) return 0;
    memcpy(tmp, arg->start, seg_len);
    tmp[seg_len] = 0;
    end = tmp + seg_len;
    if (!isnamestart(*p)) return 0;

    name_len = bc_parse_varname(p, &type_hint, &is_array_hint);
    if (name_len == 0) return 0;
    p += name_len;
    p = skipspace_to(p, end);
    if (p < end) {
        if (*p != '(') return 0;
        p++;
        p = skipspace_to(p, end);
        if (p >= end || *p != ')') return 0;
        p++;
        p = skipspace_to(p, end);
        if (p != end) return 0;
    }

    if (cs->current_subfun >= 0) {
        loc = bc_find_local(cs, (const char *)tmp, name_len);
        if (loc >= 0) {
            is_local = 1;
            slot = (uint16_t)loc;
            type = cs->locals[loc].type;
            symbol_is_array = cs->locals[loc].is_array;
        }
    }

    if (!is_local) {
        slot = bc_find_slot(cs, (const char *)tmp, name_len);
        if (slot == 0xFFFF) {
            if (type_hint == 0) return 0;
            slot = bc_add_slot(cs, (const char *)tmp, name_len, type_hint, 1);
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

    arg->slot = slot;
    arg->type = type;
    if (is_local)
        arg->kind = (type == T_INT) ? BC_BOX_ARG_LOCAL_ARR_I : BC_BOX_ARG_LOCAL_ARR_F;
    else
        arg->kind = (type == T_INT) ? BC_BOX_ARG_GLOBAL_ARR_I : BC_BOX_ARG_GLOBAL_ARR_F;
    return 1;
}

static int compile_box_native(BCCompiler *cs, unsigned char **pp) {
    BCBoxArgSpec args[BC_BOX_ARG_COUNT];
    unsigned char *p = *pp;
    unsigned char *end = p;
    int field_count = 0;
    int i;

    memset(args, 0, sizeof(args));
    for (i = 0; i < BC_BOX_ARG_COUNT; i++)
        args[i].kind = BC_BOX_ARG_EMPTY;

    if (parse_box_args(p, args, BC_BOX_ARG_COUNT, &field_count, &end) != 0) return 0;
    if (field_count < 4 || field_count > BC_BOX_ARG_COUNT) return 0;

    for (i = 0; i < field_count; i++) {
        int rc = try_parse_box_array_ref(cs, &args[i], "BOX");
        if (rc < 0) return 1;
        if (rc > 0) continue;
        if (!args[i].present) continue;
        p = args[i].start;
        args[i].type = bc_compile_expression(cs, &p);
        p = skipspace_to(p, args[i].end);
        if (p != args[i].end) {
            bc_set_error(cs, "Invalid BOX argument");
            return 1;
        }
        if ((args[i].type & (T_INT | T_NBR)) == 0 || (args[i].type & T_STR)) {
            bc_set_error(cs, "BOX requires numeric arguments");
            return 1;
        }
        args[i].kind = BC_BOX_ARG_STACK;
    }

    bc_emit_byte(cs, OP_BOX);
    bc_emit_byte(cs, (uint8_t)field_count);
    for (i = 0; i < BC_BOX_ARG_COUNT; i++) {
        bc_emit_byte(cs, args[i].kind);
        if (args[i].kind == BC_BOX_ARG_GLOBAL_ARR_I ||
            args[i].kind == BC_BOX_ARG_GLOBAL_ARR_F ||
            args[i].kind == BC_BOX_ARG_LOCAL_ARR_I ||
            args[i].kind == BC_BOX_ARG_LOCAL_ARR_F) {
            bc_emit_u16(cs, args[i].slot);
        }
    }

    while (*end) end++;
    *pp = end;
    return 1;
}

static int compile_circle_native(BCCompiler *cs, unsigned char **pp) {
    BCBoxArgSpec args[BC_BOX_ARG_COUNT];
    unsigned char *p = *pp;
    unsigned char *end = p;
    int field_count = 0;
    int i;

    memset(args, 0, sizeof(args));
    for (i = 0; i < BC_BOX_ARG_COUNT; i++)
        args[i].kind = BC_BOX_ARG_EMPTY;

    if (parse_box_args(p, args, BC_BOX_ARG_COUNT, &field_count, &end) != 0) return 0;
    if (field_count < 3 || field_count > BC_BOX_ARG_COUNT) return 0;

    for (i = 0; i < field_count; i++) {
        int rc = try_parse_box_array_ref(cs, &args[i], "CIRCLE");
        if (rc < 0) return 1;
        if (rc > 0) continue;
        if (!args[i].present) continue;
        p = args[i].start;
        args[i].type = bc_compile_expression(cs, &p);
        p = skipspace_to(p, args[i].end);
        if (p != args[i].end) {
            bc_set_error(cs, "Invalid CIRCLE argument");
            return 1;
        }
        if ((args[i].type & (T_INT | T_NBR)) == 0 || (args[i].type & T_STR)) {
            bc_set_error(cs, "CIRCLE requires numeric arguments");
            return 1;
        }
        args[i].kind = BC_BOX_ARG_STACK;
    }

    bc_emit_byte(cs, OP_CIRCLE);
    bc_emit_byte(cs, (uint8_t)field_count);
    for (i = 0; i < BC_BOX_ARG_COUNT; i++) {
        bc_emit_byte(cs, args[i].kind);
        if (args[i].kind == BC_BOX_ARG_GLOBAL_ARR_I ||
            args[i].kind == BC_BOX_ARG_GLOBAL_ARR_F ||
            args[i].kind == BC_BOX_ARG_LOCAL_ARR_I ||
            args[i].kind == BC_BOX_ARG_LOCAL_ARR_F) {
            bc_emit_u16(cs, args[i].slot);
        }
    }

    while (*end) end++;
    *pp = end;
    return 1;
}

static int compile_line_native(BCCompiler *cs, unsigned char **pp) {
    BCBoxArgSpec args[BC_LINE_ARG_COUNT];
    unsigned char *p = *pp;
    unsigned char *end = p;
    int field_count = 0;
    int i;

    if (checkstring(p, (unsigned char *)"AA") != NULL) return 0;
    if (checkstring(p, (unsigned char *)"GRAPH") != NULL) return 0;

    memset(args, 0, sizeof(args));
    for (i = 0; i < BC_LINE_ARG_COUNT; i++)
        args[i].kind = BC_BOX_ARG_EMPTY;

    if (parse_box_args(p, args, BC_LINE_ARG_COUNT, &field_count, &end) != 0) return 0;
    if (field_count < 2 || field_count > BC_LINE_ARG_COUNT) return 0;

    for (i = 0; i < field_count; i++) {
        int rc = try_parse_box_array_ref(cs, &args[i], "LINE");
        if (rc < 0) return 1;
        if (rc > 0) continue;
        if (!args[i].present) continue;
        p = args[i].start;
        args[i].type = bc_compile_expression(cs, &p);
        p = skipspace_to(p, args[i].end);
        if (p != args[i].end) {
            bc_set_error(cs, "Invalid LINE argument");
            return 1;
        }
        if ((args[i].type & (T_INT | T_NBR)) == 0 || (args[i].type & T_STR)) {
            bc_set_error(cs, "LINE requires numeric arguments");
            return 1;
        }
        args[i].kind = BC_BOX_ARG_STACK;
    }

    bc_emit_byte(cs, OP_DRAW_LINE);
    bc_emit_byte(cs, (uint8_t)field_count);
    for (i = 0; i < BC_LINE_ARG_COUNT; i++) {
        bc_emit_byte(cs, args[i].kind);
        if (args[i].kind == BC_BOX_ARG_GLOBAL_ARR_I ||
            args[i].kind == BC_BOX_ARG_GLOBAL_ARR_F ||
            args[i].kind == BC_BOX_ARG_LOCAL_ARR_I ||
            args[i].kind == BC_BOX_ARG_LOCAL_ARR_F) {
            bc_emit_u16(cs, args[i].slot);
        }
    }

    while (*end) end++;
    *pp = end;
    return 1;
}

static int try_compile_text_justification_literal(BCCompiler *cs, BCBoxArgSpec *arg) {
    unsigned char buf[STRINGSIZE];
    int len, i;
    uint16_t idx;

    if (!arg->present) return 0;
    len = (int)(arg->end - arg->start);
    if (len <= 0 || len >= STRINGSIZE) return 0;
    for (i = 0; i < len; i++) {
        unsigned char ch = arg->start[i];
        if (!(isalpha((unsigned char)ch) || ch == ' ')) return 0;
        buf[i] = ch;
    }
    buf[len] = 0;
    idx = bc_add_constant_string(cs, buf, len);
    if (idx == 0xFFFF) return -1;
    bc_emit_byte(cs, OP_PUSH_STR);
    bc_emit_u16(cs, idx);
    arg->kind = BC_BOX_ARG_STACK;
    arg->type = T_STR;
    return 1;
}

static int compile_text_native(BCCompiler *cs, unsigned char **pp) {
    BCBoxArgSpec args[BC_TEXT_ARG_COUNT];
    unsigned char *p = *pp;
    unsigned char *end = p;
    int field_count = 0;
    int i;

    memset(args, 0, sizeof(args));
    for (i = 0; i < BC_TEXT_ARG_COUNT; i++)
        args[i].kind = BC_BOX_ARG_EMPTY;

    if (parse_box_args(p, args, BC_TEXT_ARG_COUNT, &field_count, &end) != 0) return 0;
    if (field_count < 3 || field_count > BC_TEXT_ARG_COUNT) return 0;

    for (i = 0; i < field_count; i++) {
        if (!args[i].present) continue;
        if (i == 3) {
            int rc = try_compile_text_justification_literal(cs, &args[i]);
            if (rc < 0) return 1;
            if (rc > 0) continue;
        }
        p = args[i].start;
        args[i].type = bc_compile_expression(cs, &p);
        p = skipspace_to(p, args[i].end);
        if (p != args[i].end) {
            bc_set_error(cs, "Invalid TEXT argument");
            return 1;
        }
        if ((i == 0 || i == 1 || i >= 4) &&
            (((args[i].type & (T_INT | T_NBR)) == 0) || (args[i].type & T_STR))) {
            bc_set_error(cs, "TEXT requires numeric arguments");
            return 1;
        }
        if ((i == 2 || i == 3) && args[i].type != T_STR) {
            bc_set_error(cs, "TEXT requires string arguments");
            return 1;
        }
        args[i].kind = BC_BOX_ARG_STACK;
    }

    bc_emit_byte(cs, OP_TEXT);
    bc_emit_byte(cs, (uint8_t)field_count);
    for (i = 0; i < BC_TEXT_ARG_COUNT; i++)
        bc_emit_byte(cs, args[i].kind);

    while (*end) end++;
    *pp = end;
    return 1;
}

static int compile_cls_native(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp;
    int has_arg = 0;
    uint8_t type;

    skipspace(p);
    if (*p && *p != '\'') {
        has_arg = 1;
        type = bc_compile_expression(cs, &p);
        skipspace(p);
        if (*p != 0) {
            bc_set_error(cs, "Invalid CLS argument");
            return 1;
        }
        if (((type & (T_INT | T_NBR)) == 0) || (type & T_STR)) {
            bc_set_error(cs, "CLS requires numeric arguments");
            return 1;
        }
    }

    bc_emit_byte(cs, OP_CLS);
    bc_emit_byte(cs, (uint8_t)has_arg);
    while (*p) p++;
    *pp = p;
    return 1;
}

static int compile_pixel_native(BCCompiler *cs, unsigned char **pp) {
    BCBoxArgSpec args[BC_PIXEL_ARG_COUNT];
    unsigned char *p = *pp;
    unsigned char *end = p;
    int field_count = 0;
    int i;

    memset(args, 0, sizeof(args));
    for (i = 0; i < BC_PIXEL_ARG_COUNT; i++)
        args[i].kind = BC_BOX_ARG_EMPTY;

    if (parse_box_args(p, args, BC_PIXEL_ARG_COUNT, &field_count, &end) != 0) return 0;
    if (field_count < 2 || field_count > BC_PIXEL_ARG_COUNT) return 0;

    for (i = 0; i < field_count; i++) {
        int rc = try_parse_box_array_ref(cs, &args[i], "PIXEL");
        if (rc < 0) return 1;
        if (rc > 0) continue;
        if (!args[i].present) continue;
        p = args[i].start;
        args[i].type = bc_compile_expression(cs, &p);
        p = skipspace_to(p, args[i].end);
        if (p != args[i].end) {
            bc_set_error(cs, "Invalid PIXEL argument");
            return 1;
        }
        if ((args[i].type & (T_INT | T_NBR)) == 0 || (args[i].type & T_STR)) {
            bc_set_error(cs, "PIXEL requires numeric arguments");
            return 1;
        }
        args[i].kind = BC_BOX_ARG_STACK;
    }

    bc_emit_byte(cs, OP_PIXEL);
    bc_emit_byte(cs, (uint8_t)field_count);
    for (i = 0; i < BC_PIXEL_ARG_COUNT; i++) {
        bc_emit_byte(cs, args[i].kind);
        if (args[i].kind == BC_BOX_ARG_GLOBAL_ARR_I ||
            args[i].kind == BC_BOX_ARG_GLOBAL_ARR_F ||
            args[i].kind == BC_BOX_ARG_LOCAL_ARR_I ||
            args[i].kind == BC_BOX_ARG_LOCAL_ARR_F) {
            bc_emit_u16(cs, args[i].slot);
        }
    }

    while (*end) end++;
    *pp = end;
    return 1;
}

static int compile_fastgfx_native(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp;
    unsigned char *rest;
    uint8_t type;

    if (cmdline_matches_exact(p, "CREATE")) {
        bc_emit_byte(cs, OP_FASTGFX_CREATE);
    } else if (cmdline_matches_exact(p, "CLOSE")) {
        bc_emit_byte(cs, OP_FASTGFX_CLOSE);
    } else if (cmdline_matches_exact(p, "SWAP")) {
        bc_emit_byte(cs, OP_FASTGFX_SWAP);
    } else if (cmdline_matches_exact(p, "SYNC")) {
        bc_emit_byte(cs, OP_FASTGFX_SYNC);
    } else if ((rest = cmdline_match_prefix(p, "FPS")) != NULL) {
        if (*rest == 0) return 0;
        type = bc_compile_expression(cs, &rest);
        skipspace(rest);
        if (*rest != 0) {
            bc_set_error(cs, "Invalid FASTGFX FPS argument");
            return 1;
        }
        if ((type & (T_INT | T_NBR)) == 0 || (type & T_STR)) {
            bc_set_error(cs, "FASTGFX FPS requires a numeric argument");
            return 1;
        }
        bc_emit_byte(cs, OP_FASTGFX_FPS);
    } else {
        return 0;
    }
    while (*p) p++;
    *pp = p;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Unsupported built-in command handler                               */
/* ------------------------------------------------------------------ */

static void compile_builtin_cmd(BCCompiler *cs, unsigned char **pp, uint16_t cmd_token) {
    unsigned char *p = *pp;
    while (*p) p++;
    *pp = p;
    bc_set_error(cs, "Unsupported VM command: %s", commandname(cmd_token));
}

/* ------------------------------------------------------------------ */
/*  Assignment: var = expr  or  var(idx,...) = expr                     */
/* ------------------------------------------------------------------ */

void bc_compile_assignment(BCCompiler *cs, unsigned char **pp) {
    unsigned char *p = *pp; skipspace(p);
    uint8_t vtype = 0; int is_arr = 0;
    int namelen = bc_parse_varname(p, &vtype, &is_arr);
    if (namelen == 0) { bc_set_error(cs, "Expected variable in assignment"); return; }
    if (vtype == 0) {
        /* No suffix — look up the existing local or slot to get its type */
        if (cs->current_subfun >= 0) {
            int loc = bc_find_local(cs, (const char *)p, namelen);
            if (loc >= 0) vtype = cs->locals[loc].type;
        }
        if (vtype == 0) {
            uint16_t existing = bc_find_slot(cs, (const char *)p, namelen);
            if (existing != 0xFFFF) vtype = cs->slots[existing].type;
        }
        if (vtype == 0) vtype = T_NBR;  /* default to float */
    }

    int is_local = 0;
    uint16_t slot = resolve_var(cs, (const char *)p, namelen, vtype, is_arr, &is_local);
    p += namelen;

    if (is_arr && *p == '(') {
        p++;
        int ndim = 0;
        while (1) {
            skipspace(p);
            bc_compile_expression(cs, &p);
            ndim++;
            skipspace(p);
            if (*p == ',') p++; else break;
        }
        if (*p == ')') p++;
        skipspace(p);
        if (*p == '=' || *p == tokenEQUAL) p++;
        skipspace(p);
        uint8_t etype = bc_compile_expression(cs, &p);
        /* Type conversion if needed */
        if ((vtype & T_INT) && (etype & T_NBR)) bc_emit_byte(cs, OP_CVT_F2I);
        else if ((vtype & T_NBR) && (etype & T_INT)) bc_emit_byte(cs, OP_CVT_I2F);
        uint8_t op;
        if (is_local) {
            switch (vtype & (T_INT | T_NBR | T_STR)) {
                case T_INT: op = OP_STORE_LOCAL_ARR_I; break;
                case T_STR: op = OP_STORE_LOCAL_ARR_S; break;
                default:    op = OP_STORE_LOCAL_ARR_F; break;
            }
        } else {
            switch (vtype & (T_INT | T_NBR | T_STR)) {
                case T_INT: op = OP_STORE_ARR_I; break;
                case T_STR: op = OP_STORE_ARR_S; break;
                default:    op = OP_STORE_ARR_F; break;
            }
        }
        bc_emit_byte(cs, op);
        bc_emit_u16(cs, slot);
        bc_emit_byte(cs, (uint8_t)ndim);
    } else {
        skipspace(p);
        if (*p == '=' || *p == tokenEQUAL) p++;
        skipspace(p);
        uint8_t etype = bc_compile_expression(cs, &p);
        /* Type conversion if needed */
        if ((vtype & T_INT) && (etype & T_NBR)) bc_emit_byte(cs, OP_CVT_F2I);
        else if ((vtype & T_NBR) && (etype & T_INT)) bc_emit_byte(cs, OP_CVT_I2F);
        bc_emit_store_var(cs, slot, vtype, is_local);
    }
    *pp = p;
}

/* ------------------------------------------------------------------ */
/*  Main statement dispatcher                                          */
/* ------------------------------------------------------------------ */

void bc_compile_statement(BCCompiler *cs, unsigned char **pp, uint16_t cmd_token) {
    if (cs->has_error) return;

    /* IF / ELSEIF / ELSE / ENDIF */
    if (cmd_token == cmdIF)                               { compile_if(cs, pp);       return; }
    if (cmd_token == cmdELSEIF || cmd_token == cmdELSE_IF){ compile_elseif(cs, pp);   return; }
    if (cmd_token == cmdELSE)                             { compile_else(cs, pp);     return; }
    if (cmd_token == cmdENDIF || cmd_token == cmdEND_IF)  { compile_endif(cs, pp);    return; }

    /* FOR / NEXT */
    if (cmd_token == cmdFOR)  { compile_for(cs, pp);  return; }
    if (cmd_token == cmdNEXT) { compile_next(cs, pp); return; }

    /* DO / LOOP */
    if (cmd_token == cmdDO)   { compile_do(cs, pp);   return; }
    if (cmd_token == cmdLOOP) { compile_loop(cs, pp); return; }

    /* WHILE */
    if (cmd_token == cmdWHILE) { compile_while(cs, pp); return; }

    /* SUB / FUNCTION */
    if (cmd_token == cmdSUB)           { compile_sub(cs, pp);          return; }
    if (cmd_token == cmdENDSUB)        { compile_end_sub(cs, pp);      return; }
    if (cmd_token == cmdFUN)           { compile_function(cs, pp);     return; }
    if (cmd_token == cmdENDFUNCTION)   { compile_end_function(cs, pp); return; }

    /* LOCAL / STATIC */
    if (cmd_token == cmdLOCAL || cmd_token == cmdSTATIC) { compile_local(cs, pp); return; }

    /* SELECT CASE */
    if (cmd_token == cmdSELECT_CASE) { compile_select_case(cs, pp); return; }
    if (cmd_token == cmdCASE)        { compile_case(cs, pp);        return; }
    if (cmd_token == cmdCASE_ELSE)   { compile_case_else(cs, pp);   return; }
    if (cmd_token == cmdEND_SELECT)  { compile_end_select(cs, pp);  return; }

    /* Comments, CSUB, IRET, OPTION — skip (compile-time directives) */
    {
        unsigned short cmdOPTION = (unsigned short)GetCommandValue((unsigned char *)"Option");
        if (cmd_token == cmdComment || cmd_token == cmdEndComment ||
            cmd_token == cmdCSUB || cmd_token == cmdIRET ||
            cmd_token == cmdOPTION) {
            unsigned char *p = *pp; while (*p) p++; *pp = p;
            return;
        }
    }

    /* Commands not available as pre-cached globals: use static cache */
    {
        unsigned short cGOTO    = (unsigned short)GetCommandValue((unsigned char *)"GoTo");
        unsigned short cGOSUB   = (unsigned short)GetCommandValue((unsigned char *)"GoSub");
        unsigned short cRETURN  = (unsigned short)GetCommandValue((unsigned char *)"Return");
        unsigned short cPRINT   = (unsigned short)GetCommandValue((unsigned char *)"Print");
        unsigned short cDIM     = (unsigned short)GetCommandValue((unsigned char *)"Dim");
        unsigned short cEND     = (unsigned short)GetCommandValue((unsigned char *)"End");
        unsigned short cEXIT    = (unsigned short)GetCommandValue((unsigned char *)"Exit");
        unsigned short cEXITFOR = (unsigned short)GetCommandValue((unsigned char *)"Exit For");
        unsigned short cEXITDO  = (unsigned short)GetCommandValue((unsigned char *)"Exit Do");
        unsigned short cEXITFUN = (unsigned short)GetCommandValue((unsigned char *)"Exit Function");
        unsigned short cEXITSUB = (unsigned short)GetCommandValue((unsigned char *)"Exit Sub");
        unsigned short cLET     = (unsigned short)GetCommandValue((unsigned char *)"Let");
        unsigned short cDATA    = (unsigned short)GetCommandValue((unsigned char *)"Data");
        unsigned short cREAD    = (unsigned short)GetCommandValue((unsigned char *)"Read");
        unsigned short cRESTORE = (unsigned short)GetCommandValue((unsigned char *)"Restore");
        unsigned short cINC       = (unsigned short)GetCommandValue((unsigned char *)"Inc");
        unsigned short cCONST     = (unsigned short)GetCommandValue((unsigned char *)"Const");
        unsigned short cRANDOMIZE = (unsigned short)GetCommandValue((unsigned char *)"Randomize");
        unsigned short cERROR     = (unsigned short)GetCommandValue((unsigned char *)"Error");
        unsigned short cCLEAR     = (unsigned short)GetCommandValue((unsigned char *)"Clear");
        unsigned short cCLS       = (unsigned short)GetCommandValue((unsigned char *)"CLS");
        unsigned short cFASTGFX   = (unsigned short)GetCommandValue((unsigned char *)"FASTGFX");
        unsigned short cBOX       = (unsigned short)GetCommandValue((unsigned char *)"Box");
        unsigned short cCIRCLE    = (unsigned short)GetCommandValue((unsigned char *)"Circle");
        unsigned short cLINE      = (unsigned short)GetCommandValue((unsigned char *)"Line");
        unsigned short cTEXT      = (unsigned short)GetCommandValue((unsigned char *)"Text");
        unsigned short cPIXEL     = (unsigned short)GetCommandValue((unsigned char *)"Pixel");
        unsigned short cCOLOR     = (unsigned short)GetCommandValue((unsigned char *)"Color");
        unsigned short cCOLOUR    = (unsigned short)GetCommandValue((unsigned char *)"Colour");
        unsigned short cPAUSE     = (unsigned short)GetCommandValue((unsigned char *)"Pause");
        if (cmd_token == cGOTO)    { compile_goto(cs, pp);   return; }
        if (cmd_token == cGOSUB)   { compile_gosub(cs, pp);  return; }
        if (cmd_token == cRETURN)  { bc_emit_byte(cs, OP_RETURN); return; }
        if (cmd_token == cPRINT)   { compile_print(cs, pp);  return; }
        if (cmd_token == cDIM)     { compile_dim(cs, pp);     return; }
        if (cmd_token == cEND)     { bc_emit_byte(cs, OP_END); return; }
        if (cmd_token == cEXITFOR) { compile_exit_for(cs, pp); return; }
        if (cmd_token == cEXITDO)  { compile_exit_do(cs, pp);  return; }
        if (cmd_token == cEXITFUN) { compile_exit_function(cs, pp); return; }
        if (cmd_token == cEXITSUB) { compile_exit_sub(cs, pp); return; }
        if (cmd_token == cEXIT)    { compile_exit(cs, pp);   return; }
        if (cmd_token == cLET)     { bc_compile_assignment(cs, pp); return; }
        if (cmd_token == cDATA)    { compile_data(cs, pp);    return; }
        if (cmd_token == cREAD)    { compile_read(cs, pp);    return; }
        if (cmd_token == cRESTORE) { compile_restore(cs, pp); return; }
        if (cmd_token == cINC)       { compile_inc(cs, pp);       return; }
        if (cmd_token == cCONST)     { compile_const(cs, pp);     return; }
        if (cmd_token == cRANDOMIZE) { compile_randomize(cs, pp); return; }
        if (cmd_token == cERROR)     { compile_error(cs, pp);     return; }
        if (cmd_token == cCLEAR)     { compile_clear(cs, pp);     return; }
        if (cmd_token == cCLS && compile_cls_native(cs, pp)) { return; }
        if (cmd_token == cFASTGFX && compile_fastgfx_native(cs, pp)) { return; }
        if (cmd_token == cBOX && compile_box_native(cs, pp)) { return; }
        if (cmd_token == cCIRCLE && compile_circle_native(cs, pp)) { return; }
        if (cmd_token == cLINE && compile_line_native(cs, pp)) { return; }
        if (cmd_token == cTEXT && compile_text_native(cs, pp)) { return; }
        if (cmd_token == cPIXEL && compile_pixel_native(cs, pp)) { return; }
        if (cmd_token == cCOLOR || cmd_token == cCOLOUR) {
            while (**pp) (*pp)++;
            bc_emit_byte(cs, OP_COLOUR);
            return;
        }
        if (cmd_token == cPAUSE) {
            uint8_t type = bc_compile_expression(cs, pp);
            if (cs->has_error) return;
            if ((type & (T_INT | T_NBR)) == 0 || (type & T_STR)) {
                bc_set_error(cs, "PAUSE requires a numeric argument");
                return;
            }
            bc_emit_byte(cs, OP_PAUSE);
            return;
        }
    }

    /* Default: unsupported built-in command */
    compile_builtin_cmd(cs, pp, cmd_token);
}
