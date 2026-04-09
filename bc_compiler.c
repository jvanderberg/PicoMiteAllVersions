/*
 * bc_compiler.c — Top-level bytecode compiler for FRUN command
 *
 * Implements bc_compile(): two-pass compilation from tokenized ProgMemory
 * to bytecode.
 *
 *  Pass 1 — Scan: find all SUB/FUNCTION definitions and record their
 *           names, parameter lists, and entry points.  Also scan DIM
 *           statements to pre-populate the slot table.
 *
 *  Pass 2 — Emit: walk the tokenized program again, emitting bytecode
 *           for each statement.  Expressions are compiled by
 *           bc_compile_expression() (bc_compiler_expr.c) and statements
 *           by bc_compile_statement() (bc_compiler_stmt.c).
 *
 *  After pass 2, resolve all forward-reference fixups using the linemap.
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include "bytecode.h"
#include "bc_compiler_internal.h"

/* External MMBasic globals */
extern unsigned char *ProgMemory;
extern int PSize;

/* ------------------------------------------------------------------ */
/*  Pass 1 helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * Parse a parameter list  "(name%, name!, ...)"  from the tokenized
 * stream and fill in a BCSubFun record's parameter info.
 * Returns the pointer past the closing ')'.
 */
static unsigned char *scan_params(BCCompiler *cs, BCSubFun *sf, unsigned char *p) {
    sf->nparams = 0;

    skipspace(p);
    if (*p != '(') return p;  /* no parameters */
    p++;  /* skip '(' */
    skipspace(p);

    if (*p == ')') { p++; return p; }  /* empty parens */

    while (1) {
        skipspace(p);
        if (!isnamestart(*p)) break;

        uint8_t ptype = 0;
        int is_arr = 0;
        int len = bc_parse_varname(p, &ptype, &is_arr);
        if (len == 0) break;

        if (ptype == 0) {
            bc_set_error(cs, "Parameter must have explicit type suffix");
            return p;
        }

        if (sf->nparams < BC_MAX_LOCALS) {
            sf->param_types[sf->nparams] = ptype;
            sf->param_is_array[sf->nparams] = (uint8_t)is_arr;
            sf->nparams++;
        }

        p += len;
        if (is_arr) {
            /* skip past the () */
            if (*p == '(') {
                int depth = 1;
                p++;
                while (depth > 0 && *p) {
                    if (*p == '(') depth++;
                    else if (*p == ')') depth--;
                    p++;
                }
            }
        }

        skipspace(p);
        if (*p == ',') { p++; continue; }
        if (*p == ')') { p++; break; }
        break;
    }

    return p;
}

/*
 * Pass 1: scan for SUB/FUNCTION definitions and DIM statements.
 * Populates cs->subfuns[] and cs->slots[].
 */
static void pass1_scan(BCCompiler *cs, unsigned char *prog, int prog_size) {
    unsigned char *p = prog;
    unsigned char *end = prog + prog_size;

    while (p < end) {
        /* Skip to start of element */
        if (*p == 0) { p++; continue; }

        if (*p == T_NEWLINE) {
            p++;
            continue;
        }
        if (*p == T_LINENBR) {
            p += 3;
            skipspace(p);
            continue;
        }

        /* Skip labels */
        if (*p == T_LABEL) {
            p += p[1] + 2;
            skipspace(p);
            continue;
        }

        /* End of program */
        if (*p == 0 && (p + 1 >= end || p[1] == 0)) break;

        /* Check for command token */
        if (bc_is_cmd_token(p)) {
            uint16_t cmd = bc_decode_cmd(p);
            unsigned char *args = p + 2;  /* sizeof(CommandToken) == 2 */
            skipspace(args);

            if (cmd == cmdSUB || cmd == cmdFUN) {
                /* Parse SUB/FUNCTION name and parameters */
                unsigned char *np = args;
                if (isnamestart(*np)) {
                    int name_len = 0;
                    while (isnamechar(np[name_len])) name_len++;

                    /* For FUNCTION, check return type suffix */
                    uint8_t ret_type = 0;
                    if (np[name_len] == '%' || np[name_len] == '!' || np[name_len] == '$') {
                        ret_type = bc_type_from_suffix(np[name_len]);
                        name_len++;
                    }

                    /* Add to subfun table */
                    int idx = bc_find_subfun(cs, (const char *)np, name_len - (ret_type ? 1 : 0));
                    if (idx < 0 && cs->subfun_count < BC_MAX_SUBFUNS) {
                        idx = cs->subfun_count++;
                        int copy_len = name_len - (ret_type ? 1 : 0);
                        if (copy_len > MAXVARLEN) copy_len = MAXVARLEN;
                        memcpy(cs->subfuns[idx].name, np, copy_len);
                        cs->subfuns[idx].name[copy_len] = '\0';
                        cs->subfuns[idx].return_type = (cmd == cmdFUN) ? ret_type : 0;
                        cs->subfuns[idx].entry_addr = 0;  /* filled in pass 2 */
                        cs->subfuns[idx].nlocals = 0;

                        /* Parse parameter list */
                        unsigned char *pp = np + name_len;
                        skipspace(pp);
                        scan_params(cs, &cs->subfuns[idx], pp);
                    }
                }
            }
        }

        /* Skip to end of element */
        skipelement(p);
        if (*p == 0) p++;  /* step over terminating zero */
    }
}


/* ------------------------------------------------------------------ */
/*  Pass 2: emit bytecode                                              */
/* ------------------------------------------------------------------ */

static void pass2_emit(BCCompiler *cs, unsigned char *prog, int prog_size) {
    unsigned char *p = prog;
    unsigned char *end = prog + prog_size;
    int line_count = 0;

    while (p < end && !cs->has_error) {
        /* Skip element separator */
        if (*p == 0) { p++; continue; }

        /* New line marker */
        if (*p == T_NEWLINE) {
            p++;
            continue;
        }

        /* Line number */
        if (*p == T_LINENBR) {
            /* MMBasic T_LINENBR is big-endian: p[1]=high, p[2]=low */
            uint16_t lineno = ((uint16_t)(p[1]) << 8) | (uint16_t)(p[2]);
            cs->current_line = lineno;

            /* Add to line map */
            if (cs->linemap_count < BC_MAX_LINEMAP) {
                cs->linemap[cs->linemap_count].lineno = lineno;
                cs->linemap[cs->linemap_count].offset = cs->code_len;
                cs->linemap_count++;
            }

            /* Emit OP_LINE for error reporting */
            bc_emit_byte(cs, OP_LINE);
            bc_emit_u16(cs, lineno);

            /* Periodic interrupt check */
            line_count++;
            if ((line_count & 0x1F) == 0) {
                bc_emit_byte(cs, OP_CHECKINT);
            }

            p += 3;
            skipspace(p);
            continue;
        }

        /* Skip labels */
        if (*p == T_LABEL) {
            p += p[1] + 2;
            skipspace(p);
            continue;
        }

        /* End of program */
        if (*p == 0 && (p + 1 >= end || p[1] == 0)) break;
        if (*p == 0xFF && (p + 1 >= end || p[1] == 0xFF)) break;

        /* Comment line (apostrophe) */
        if (*p == '\'') {
            skipelement(p);
            if (*p == 0) p++;
            continue;
        }

        /* Check for a command token (2 bytes, both >= C_BASETOKEN) */
        if (bc_is_cmd_token(p)) {
            uint16_t cmd = bc_decode_cmd(p);
            unsigned char *args = p + 2;
            skipspace(args);

            /* Skip comment blocks */
            if (cmd == cmdComment) {
                /* block comment start -- skip until cmdEndComment */
                skipelement(p);
                if (*p == 0) p++;
                continue;
            }
            if (cmd == cmdEndComment) {
                skipelement(p);
                if (*p == 0) p++;
                continue;
            }

            /* Dispatch to statement compiler */
            bc_compile_statement(cs, &args, cmd);

            /* Advance past element */
            skipelement(p);
            if (*p == 0) p++;
            continue;
        }

        /* Not a command token — could be assignment (var = expr) or
         * user-defined sub call */
        if (isnamestart(*p)) {
            /* Check for bare SUB call: name followed by args or end-of-element */
            unsigned char *probe = p;
            int nlen = 0;
            while (isnamechar(probe[nlen])) nlen++;
            /* Skip type suffix if present */
            int nlen_base = nlen;
            if (nlen > 0 && (probe[nlen] == '%' || probe[nlen] == '!' || probe[nlen] == '$')) {
                /* Variables have type suffixes, SUBs don't — skip */
            } else {
                int sf_idx = bc_find_subfun(cs, (const char *)probe, nlen);
                if (sf_idx >= 0 && cs->subfuns[sf_idx].return_type == 0) {
                    /* It's a SUB call */
                    p += nlen;
                    skipspace(p);
                    int nargs = 0;
                    if (*p == '(' || (*p && *p != '\0' && *p != '\'')) {
                        /* Has arguments — compile them */
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
                    /* Advance to end of element */
                    if (*p != 0) skipelement(p);
                    if (*p == 0) p++;
                    continue;
                }
            }

            unsigned char *save = p;
            bc_compile_assignment(cs, &p);

            /* If assignment didn't consume anything, skip the element */
            if (p == save) {
                skipelement(p);
                if (*p == 0) p++;
            } else {
                /* Advance to end of element if not already there */
                if (*p != 0) {
                    skipelement(p);
                }
                if (*p == 0) p++;
            }
            continue;
        }

        /* Skip unknown content */
        skipelement(p);
        if (*p == 0) p++;
    }
}


/* ------------------------------------------------------------------ */
/*  Fixup resolution                                                   */
/* ------------------------------------------------------------------ */

static void resolve_fixups(BCCompiler *cs) {
    for (uint16_t i = 0; i < cs->fixup_count; i++) {
        BCFixup *f = &cs->fixups[i];

        uint32_t target;
        if (f->target_line >= 0) {
            target = bc_linemap_lookup(cs, (uint16_t)f->target_line);
            if (target == 0xFFFFFFFF) {
                bc_set_error(cs, "Undefined line number %d", f->target_line);
                return;
            }
        } else {
            /* Label-based fixup (not used yet) */
            bc_set_error(cs, "Label fixup not implemented");
            return;
        }

        if (f->is_relative) {
            int32_t offset = (int32_t)target - (int32_t)(f->patch_addr + f->size);
            if (f->size == 2) {
                if (offset < -32768 || offset > 32767) {
                    bc_set_error(cs, "Relative jump out of range (line %d)", f->target_line);
                    return;
                }
                bc_patch_i16(cs, f->patch_addr, (int16_t)offset);
            } else {
                bc_patch_u32(cs, f->patch_addr, (uint32_t)offset);
            }
        } else {
            /* Absolute address */
            if (f->size == 4) {
                bc_patch_u32(cs, f->patch_addr, target);
            } else {
                bc_patch_u16(cs, f->patch_addr, (uint16_t)target);
            }
        }
    }
}


/* ------------------------------------------------------------------ */
/*  Public API: bc_compile()                                           */
/* ------------------------------------------------------------------ */

int bc_compile(BCCompiler *cs, unsigned char *prog_memory, int prog_size) {
    /* Pass 1: scan for SUB/FUNCTION and DIM */
    pass1_scan(cs, prog_memory, prog_size);
    if (cs->has_error) return -1;

    /* Pass 2: emit bytecode */
    pass2_emit(cs, prog_memory, prog_size);
    if (cs->has_error) return -1;

    /* Always emit OP_END — can't check last byte because data bytes
     * (e.g. FOR_NEXT loop_off) may coincidentally equal 0xFE */
    bc_emit_byte(cs, OP_END);

    /* Resolve forward references */
    resolve_fixups(cs);
    if (cs->has_error) return -1;

    return 0;  /* success */
}
