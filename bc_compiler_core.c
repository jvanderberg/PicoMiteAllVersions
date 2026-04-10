/*
 * bc_compiler_core.c — Core compiler helpers: emission, slots, locals,
 *                       constants, nesting stack, fixups, line map, etc.
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include "bytecode.h"
#include "bc_compiler_internal.h"

/* ------------------------------------------------------------------ */
/*  Dynamic allocation for compiler arrays                            */
/*  Host: calloc/free (MMBasic heap uses 32-bit pointer math)         */
/*  Device: GetMemory/FreeMemory (MMBasic heap)                       */
/* ------------------------------------------------------------------ */

#ifdef MMBASIC_HOST
#include <stdlib.h>
#define BC_ALLOC(sz)   calloc(1, (sz))
#define BC_FREE(p)     free((p))
#else
extern void *GetMemory(int size);
extern void FreeMemory(unsigned char *addr);
#define BC_ALLOC(sz)   GetMemory((sz))
#define BC_FREE(p)     FreeMemory((unsigned char *)(p))
#endif

int bc_compiler_alloc(BCCompiler *cs) {
    memset(cs, 0, sizeof(*cs));
    cs->code       = (uint8_t *)BC_ALLOC(BC_MAX_CODE);
    cs->constants  = (BCConstant *)BC_ALLOC(BC_MAX_CONSTANTS * sizeof(BCConstant));
    cs->slots      = (BCSlot *)BC_ALLOC(BC_MAX_SLOTS * sizeof(BCSlot));
    cs->subfuns    = (BCSubFun *)BC_ALLOC(BC_MAX_SUBFUNS * sizeof(BCSubFun));
    cs->subfun_locals_base = (uint16_t *)BC_ALLOC(BC_MAX_SUBFUNS * sizeof(uint16_t));
    cs->fixups     = (BCFixup *)BC_ALLOC(BC_MAX_FIXUPS * sizeof(BCFixup));
    cs->linemap    = (BCLineMap *)BC_ALLOC(BC_MAX_LINEMAP * sizeof(BCLineMap));
    cs->nest_stack = (BCNestEntry *)BC_ALLOC(BC_MAX_NEST * sizeof(BCNestEntry));
    cs->locals     = (BCLocalVar *)BC_ALLOC(BC_MAX_LOCALS * sizeof(BCLocalVar));
    cs->local_meta = (BCLocalMeta *)BC_ALLOC(BC_MAX_LOCAL_META * sizeof(BCLocalMeta));
    cs->data_pool  = (BCDataItem *)BC_ALLOC(BC_MAX_DATA_ITEMS * sizeof(BCDataItem));
    if (!cs->code || !cs->constants || !cs->slots || !cs->subfuns ||
        !cs->subfun_locals_base ||
        !cs->fixups || !cs->linemap || !cs->nest_stack || !cs->locals ||
        !cs->local_meta ||
        !cs->data_pool) {
        bc_compiler_free(cs);
        return -1;
    }
    cs->current_subfun = -1;
    return 0;
}

void bc_compiler_free(BCCompiler *cs) {
    if (cs->code)       BC_FREE(cs->code);
    if (cs->constants)  BC_FREE(cs->constants);
    if (cs->slots)      BC_FREE(cs->slots);
    if (cs->subfuns)    BC_FREE(cs->subfuns);
    if (cs->subfun_locals_base) BC_FREE(cs->subfun_locals_base);
    if (cs->fixups)     BC_FREE(cs->fixups);
    if (cs->linemap)    BC_FREE(cs->linemap);
    if (cs->nest_stack) BC_FREE(cs->nest_stack);
    if (cs->locals)     BC_FREE(cs->locals);
    if (cs->local_meta) BC_FREE(cs->local_meta);
    if (cs->data_pool)  BC_FREE(cs->data_pool);
    memset(cs, 0, sizeof(*cs));
}

/* ------------------------------------------------------------------ */
/*  Compiler initialisation (reset state, arrays must be allocated)   */
/* ------------------------------------------------------------------ */

void bc_compiler_init(BCCompiler *cs) {
    cs->code_len         = 0;
    cs->const_count      = 0;
    cs->slot_count       = 0;
    cs->next_hidden_slot = 0;
    cs->subfun_count     = 0;
    if (cs->subfun_locals_base) memset(cs->subfun_locals_base, 0, BC_MAX_SUBFUNS * sizeof(uint16_t));
    cs->fixup_count      = 0;
    cs->linemap_count    = 0;
    cs->nest_depth       = 0;
    cs->current_subfun   = -1;
    cs->current_line     = 0;
    cs->local_count      = 0;
    cs->local_meta_count = 0;
    cs->data_count       = 0;
    cs->error_line       = 0;
    cs->error_msg[0]     = '\0';
    cs->has_error        = 0;
}

/* ------------------------------------------------------------------ */
/*  Bytecode emission helpers                                         */
/* ------------------------------------------------------------------ */

void bc_emit_byte(BCCompiler *cs, uint8_t b) {
    if (cs->code_len >= BC_MAX_CODE) {
        bc_set_error(cs, "Bytecode overflow (%d bytes)", BC_MAX_CODE);
        return;
    }
    cs->code[cs->code_len++] = b;
}

void bc_emit_u16(BCCompiler *cs, uint16_t v) {
    if (cs->code_len + 2 > BC_MAX_CODE) {
        bc_set_error(cs, "Bytecode overflow (%d bytes)", BC_MAX_CODE);
        return;
    }
    memcpy(&cs->code[cs->code_len], &v, 2);
    cs->code_len += 2;
}

void bc_emit_i16(BCCompiler *cs, int16_t v) {
    if (cs->code_len + 2 > BC_MAX_CODE) {
        bc_set_error(cs, "Bytecode overflow (%d bytes)", BC_MAX_CODE);
        return;
    }
    memcpy(&cs->code[cs->code_len], &v, 2);
    cs->code_len += 2;
}

void bc_emit_u32(BCCompiler *cs, uint32_t v) {
    if (cs->code_len + 4 > BC_MAX_CODE) {
        bc_set_error(cs, "Bytecode overflow (%d bytes)", BC_MAX_CODE);
        return;
    }
    memcpy(&cs->code[cs->code_len], &v, 4);
    cs->code_len += 4;
}

void bc_emit_ptr(BCCompiler *cs, const void *ptr) {
    uintptr_t v = (uintptr_t)ptr;
    if (cs->code_len + (uint32_t)sizeof(v) > BC_MAX_CODE) {
        bc_set_error(cs, "Bytecode overflow (%d bytes)", BC_MAX_CODE);
        return;
    }
    memcpy(&cs->code[cs->code_len], &v, sizeof(v));
    cs->code_len += sizeof(v);
}

void bc_emit_i64(BCCompiler *cs, int64_t v) {
    if (cs->code_len + 8 > BC_MAX_CODE) {
        bc_set_error(cs, "Bytecode overflow (%d bytes)", BC_MAX_CODE);
        return;
    }
    memcpy(&cs->code[cs->code_len], &v, 8);
    cs->code_len += 8;
}

void bc_emit_f64(BCCompiler *cs, MMFLOAT v) {
    if (cs->code_len + (uint32_t)sizeof(MMFLOAT) > BC_MAX_CODE) {
        bc_set_error(cs, "Bytecode overflow (%d bytes)", BC_MAX_CODE);
        return;
    }
    memcpy(&cs->code[cs->code_len], &v, sizeof(MMFLOAT));
    cs->code_len += sizeof(MMFLOAT);
}

/* ------------------------------------------------------------------ */
/*  Bytecode patching helpers (write at an arbitrary address)          */
/* ------------------------------------------------------------------ */

void bc_patch_u16(BCCompiler *cs, uint32_t addr, uint16_t v) {
    if (addr + 2 > cs->code_len) {
        bc_set_error(cs, "Patch address %u out of range (code_len=%u)", addr, cs->code_len);
        return;
    }
    memcpy(&cs->code[addr], &v, 2);
}

void bc_patch_i16(BCCompiler *cs, uint32_t addr, int16_t v) {
    if (addr + 2 > cs->code_len) {
        bc_set_error(cs, "Patch address %u out of range (code_len=%u)", addr, cs->code_len);
        return;
    }
    memcpy(&cs->code[addr], &v, 2);
}

void bc_patch_u32(BCCompiler *cs, uint32_t addr, uint32_t v) {
    if (addr + 4 > cs->code_len) {
        bc_set_error(cs, "Patch address %u out of range (code_len=%u)", addr, cs->code_len);
        return;
    }
    memcpy(&cs->code[addr], &v, 4);
}

/* ------------------------------------------------------------------ */
/*  Variable slot management (global compile-time slots)              */
/* ------------------------------------------------------------------ */

uint16_t bc_find_slot(BCCompiler *cs, const char *name, int name_len) {
    for (uint16_t i = 0; i < cs->slot_count; i++) {
        if ((int)strlen(cs->slots[i].name) == name_len &&
            strncasecmp(cs->slots[i].name, name, name_len) == 0) {
            return i;
        }
    }
    return 0xFFFF;
}

uint16_t bc_add_slot(BCCompiler *cs, const char *name, int name_len, uint8_t type, int is_array) {
    if (cs->slot_count >= BC_MAX_SLOTS) {
        bc_set_error(cs, "Too many variable slots (max %d)", BC_MAX_SLOTS);
        return 0xFFFF;
    }
    uint16_t idx = cs->slot_count++;
    int copy_len = name_len;
    if (copy_len > MAXVARLEN) copy_len = MAXVARLEN;
    memcpy(cs->slots[idx].name, name, copy_len);
    cs->slots[idx].name[copy_len] = '\0';
    cs->slots[idx].type     = type;
    cs->slots[idx].is_array = (uint8_t)is_array;
    cs->slots[idx].ndims    = 0;
    memset(cs->slots[idx].dims, 0, sizeof(cs->slots[idx].dims));
    return idx;
}

/* ------------------------------------------------------------------ */
/*  Local variable management (per SUB/FUNCTION scope)                */
/* ------------------------------------------------------------------ */

int bc_find_local(BCCompiler *cs, const char *name, int name_len) {
    for (int i = 0; i < (int)cs->local_count; i++) {
        if ((int)strlen(cs->locals[i].name) == name_len &&
            strncasecmp(cs->locals[i].name, name, name_len) == 0) {
            return i;
        }
    }
    return -1;
}

int bc_add_local(BCCompiler *cs, const char *name, int name_len, uint8_t type, int is_array) {
    if (cs->local_count >= BC_MAX_LOCALS) {
        bc_set_error(cs, "Too many local variables (max %d)", BC_MAX_LOCALS);
        return -1;
    }
    int idx = (int)cs->local_count++;
    int copy_len = name_len;
    if (copy_len > MAXVARLEN) copy_len = MAXVARLEN;
    memcpy(cs->locals[idx].name, name, copy_len);
    cs->locals[idx].name[copy_len] = '\0';
    cs->locals[idx].type     = type;
    cs->locals[idx].is_array = (uint8_t)is_array;
    return idx;
}

int bc_commit_locals(BCCompiler *cs, int sf_idx) {
    if (sf_idx < 0 || sf_idx >= (int)cs->subfun_count) {
        bc_set_error(cs, "Invalid SUB/FUNCTION local metadata");
        return -1;
    }
    if ((uint32_t)cs->local_meta_count + (uint32_t)cs->local_count > BC_MAX_LOCAL_META) {
        bc_set_error(cs, "Too many local metadata entries (max %d)", BC_MAX_LOCAL_META);
        return -1;
    }
    cs->subfun_locals_base[sf_idx] = cs->local_meta_count;
    for (int i = 0; i < (int)cs->local_count; i++) {
        memcpy(&cs->local_meta[cs->local_meta_count + i], &cs->locals[i], sizeof(BCLocalMeta));
    }
    cs->local_meta_count += cs->local_count;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Constant pool                                                     */
/* ------------------------------------------------------------------ */

uint16_t bc_add_constant_string(BCCompiler *cs, const uint8_t *data, int len) {
    /* Check for a duplicate first */
    for (uint16_t i = 0; i < cs->const_count; i++) {
        if (cs->constants[i].len == (uint16_t)len &&
            memcmp(cs->constants[i].data, data, len) == 0) {
            return i;
        }
    }

    if (cs->const_count >= BC_MAX_CONSTANTS) {
        bc_set_error(cs, "Too many string constants (max %d)", BC_MAX_CONSTANTS);
        return 0xFFFF;
    }

    uint16_t idx = cs->const_count++;
    int copy_len = len;
    if (copy_len > STRINGSIZE - 1) copy_len = STRINGSIZE - 1;
    memcpy(cs->constants[idx].data, data, copy_len);
    cs->constants[idx].data[copy_len] = '\0';
    cs->constants[idx].len = (uint16_t)copy_len;
    return idx;
}

/* ------------------------------------------------------------------ */
/*  SUB/FUNCTION management                                           */
/* ------------------------------------------------------------------ */

int bc_find_subfun(BCCompiler *cs, const char *name, int name_len) {
    for (int i = 0; i < (int)cs->subfun_count; i++) {
        if ((int)strlen(cs->subfuns[i].name) == name_len &&
            strncasecmp(cs->subfuns[i].name, name, name_len) == 0) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Line map                                                          */
/* ------------------------------------------------------------------ */

int bc_add_linemap_entry(BCCompiler *cs, uint16_t lineno, uint32_t offset) {
    if (cs->linemap_count >= BC_MAX_LINEMAP) {
        bc_set_error(cs, "Too many line map entries (max %d)", BC_MAX_LINEMAP);
        return -1;
    }
    cs->linemap[cs->linemap_count].lineno = lineno;
    cs->linemap[cs->linemap_count].offset = offset;
    cs->linemap_count++;
    return 0;
}

uint32_t bc_linemap_lookup(BCCompiler *cs, uint16_t lineno) {
    /* Linear search — linemap is in program order, not sorted by line number
     * (user-provided line numbers can appear in any order). */
    for (int i = 0; i < (int)cs->linemap_count; i++) {
        if (cs->linemap[i].lineno == lineno) {
            return cs->linemap[i].offset;
        }
    }
    return 0xFFFFFFFF;
}

/* ------------------------------------------------------------------ */
/*  Nesting stack helpers                                             */
/* ------------------------------------------------------------------ */

void bc_nest_push(BCCompiler *cs, BCNestType type) {
    if (cs->nest_depth >= BC_MAX_NEST) {
        bc_set_error(cs, "Nesting too deep (max %d)", BC_MAX_NEST);
        return;
    }
    BCNestEntry *e = &cs->nest_stack[cs->nest_depth];
    memset(e, 0, sizeof(*e));
    e->type = type;
    cs->nest_depth++;
}

BCNestEntry *bc_nest_top(BCCompiler *cs) {
    if (cs->nest_depth <= 0) return NULL;
    return &cs->nest_stack[cs->nest_depth - 1];
}

BCNestEntry *bc_nest_find(BCCompiler *cs, BCNestType type) {
    for (int i = cs->nest_depth - 1; i >= 0; i--) {
        if (cs->nest_stack[i].type == type) {
            return &cs->nest_stack[i];
        }
    }
    return NULL;
}

void bc_nest_pop(BCCompiler *cs) {
    if (cs->nest_depth <= 0) {
        bc_set_error(cs, "Nesting stack underflow");
        return;
    }
    cs->nest_depth--;
}

/* ------------------------------------------------------------------ */
/*  Fixup management                                                  */
/* ------------------------------------------------------------------ */

void bc_add_fixup_line(BCCompiler *cs, uint32_t patch_addr, int target_line,
                       uint8_t size, uint8_t is_relative) {
    if (cs->fixup_count >= BC_MAX_FIXUPS) {
        bc_set_error(cs, "Too many fixups (max %d)", BC_MAX_FIXUPS);
        return;
    }
    BCFixup *f = &cs->fixups[cs->fixup_count++];
    f->patch_addr   = patch_addr;
    f->target_line  = target_line;
    f->target_label = -1;
    f->size         = size;
    f->is_relative  = is_relative;
}

/* ------------------------------------------------------------------ */
/*  Variable emit helpers                                             */
/* ------------------------------------------------------------------ */

void bc_emit_load_var(BCCompiler *cs, uint16_t slot, uint8_t type, int is_local) {
    if (is_local) {
        switch (type) {
            case T_INT: bc_emit_byte(cs, OP_LOAD_LOCAL_I); break;
            case T_NBR: bc_emit_byte(cs, OP_LOAD_LOCAL_F); break;
            case T_STR: bc_emit_byte(cs, OP_LOAD_LOCAL_S); break;
            default:
                bc_set_error(cs, "Unknown type 0x%02X in load local", type);
                return;
        }
    } else {
        switch (type) {
            case T_INT: bc_emit_byte(cs, OP_LOAD_I); break;
            case T_NBR: bc_emit_byte(cs, OP_LOAD_F); break;
            case T_STR: bc_emit_byte(cs, OP_LOAD_S); break;
            default:
                bc_set_error(cs, "Unknown type 0x%02X in load global", type);
                return;
        }
    }
    bc_emit_u16(cs, slot);
}

void bc_emit_store_var(BCCompiler *cs, uint16_t slot, uint8_t type, int is_local) {
    if (is_local) {
        switch (type) {
            case T_INT: bc_emit_byte(cs, OP_STORE_LOCAL_I); break;
            case T_NBR: bc_emit_byte(cs, OP_STORE_LOCAL_F); break;
            case T_STR: bc_emit_byte(cs, OP_STORE_LOCAL_S); break;
            default:
                bc_set_error(cs, "Unknown type 0x%02X in store local", type);
                return;
        }
    } else {
        switch (type) {
            case T_INT: bc_emit_byte(cs, OP_STORE_I); break;
            case T_NBR: bc_emit_byte(cs, OP_STORE_F); break;
            case T_STR: bc_emit_byte(cs, OP_STORE_S); break;
            default:
                bc_set_error(cs, "Unknown type 0x%02X in store global", type);
                return;
        }
    }
    bc_emit_u16(cs, slot);
}

/* ------------------------------------------------------------------ */
/*  bc_skip_var — skip past a variable reference in tokenized stream  */
/* ------------------------------------------------------------------ */

unsigned char *bc_skip_var(unsigned char *p) {
    /* Skip the name characters */
    if (!isnamestart(*p)) return p;
    while (isnamechar(*p)) p++;

    /* Skip optional type suffix */
    if (*p == '%' || *p == '!' || *p == '$') p++;

    /* If followed by '(' we have array indices — skip balanced parens */
    if (*p == '(') {
        int depth = 0;
        do {
            if (*p == '(') {
                depth++;
            } else if (*p == ')') {
                depth--;
            } else if (*p == '\0') {
                /* Unterminated — bail out to avoid runaway */
                break;
            }
            p++;
        } while (depth > 0);
    }

    return p;
}
