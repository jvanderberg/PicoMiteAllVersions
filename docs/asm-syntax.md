# Inline Assembly (`'!ASM` / `'!ENDASM`)

## Overview

The `'!ASM` directive exposes the register micro-op engine (normally only
reachable via the `'!FAST` auto-optimizer) as a hand-written assembly language.
It shares the same runtime executor (`OP_FAST_LOOP` in `bc_vm.c`) — the only
new code is a mini-assembler in `bc_source.c` that parses the text and emits
the binary encoding directly.

## Syntax

```basic
SUB Example(param%)
  LOCAL x%, y%, temp%
  '!ASM
  .const LIMIT, 1000

      mov    x, 0
      mov    y, 0
  .loop:
      addi   temp, x, y
      jge    temp, LIMIT, .done
      addi   x, x, 1
      checkint
      jmp    .loop
  .done:
      exit
  '!ENDASM
  PRINT x%
END SUB
```

## Block boundaries

`'!ASM` must appear on its own line (like `'!FAST`).  All subsequent source
lines are consumed as assembly until a line containing `'!ENDASM` is reached.
Both directives look like comments to a stock interpreter, preserving
backwards compatibility of source files.

`'!ASM` is **not** attached to a DO/WHILE loop.  It stands alone and emits a
single `OP_FAST_LOOP` block at the current code position.

Multiple `'!ASM` blocks in the same SUB/FUNCTION are allowed.  Each emits
a separate `OP_FAST_LOOP` opcode with its own constant pool and label
namespace.  The register-to-local mapping is identical across blocks
(same frame), and locals are loaded/written back independently for each
block.

## Requirements

- Must be inside a `SUB` or `FUNCTION` (locals required for register mapping).
- `'!ASM` must not appear inside a `'!FAST`-annotated loop.  The
  auto-converter does not understand `OP_FAST_LOOP` opcodes in its input.

## Operands

Instruction operands can be:

- **Variable names** — local variables and parameters, without the type
  suffix.  `zr%` in BASIC becomes `zr` in assembly.  The compiler resolves
  the name to its frame slot (register index).  Case-insensitive.
- **Constant names** — names declared with `.const`.  The compiler resolves
  the name to the register holding that constant.
- **Integer literals** — decimal integer values (e.g. `0`, `-1`, `42`).
  These are auto-interned into the constant pool.  If the same value
  appears in a `.const` or was already used as a literal, the existing
  register is reused.  Negative literals are supported — the leading `-`
  is part of the literal token, not an operator (operands are
  comma-separated so there is no ambiguity).
- **Float literals** — decimal values with a `.` (e.g. `3.14`, `-0.5`).
  Auto-interned like integer literals.  Deduplication uses exact bit
  equality — `3.14` and `3.140` are the same value and share a register.
- **Labels** — for jump targets (e.g. `.loop`, `.done`).  See Labels below.
- **Array names** — for array instructions, the `.array`-declared name
  (without suffix or parens).

**Name resolution order:** when an operand is a name (not a literal or
label), the assembler resolves it in this order:

1. `.const` names — checked first.
2. Local variable names (params + LOCAL, suffix-stripped).

If a `.const` name collides with a local variable name, the constant
wins.  To avoid confusion, don't name constants the same as variables.

**Destination operand validation:** the assembler rejects instructions
where the destination register resolves to a constant.  Constants are
immutable — writing to them would silently corrupt subsequent reads
within the block.

## Directives

### `.const name, value`

Declares a named constant and assigns it a register (after all locals).

```
.const BITS, 20              ; integer constant
.const PI, 3.14159           ; float constant
.const NEG1, -1              ; integer (has sign)
```

Rules:
- Value is parsed as integer if it contains no decimal point, float otherwise.
- Constants are loaded into the register file at block entry and never
  written back — they are immutable.
- Duplicate values share a register (whether from `.const` or literals).

### `.array name`

Declares a 1D array for use with `loadi.a`/`storei.a`/`loadf.a`/`storef.a`
instructions.

```
.array buf%()                ; declares array, referenced as "buf" in instructions
.array data!()               ; declares array, referenced as "data" in instructions
```

Rules:
- The name must match a `LOCAL` or global `DIM` 1D array variable
  (including type suffix and parens).  The full BASIC name is required in
  the directive so the compiler can unambiguously match it against
  declarations (e.g. `buf%()` vs `buf!()`).
- In instructions, arrays are referenced by bare name (no suffix/parens):
  `loadi.a temp, buf, i`.
- Only 1D arrays are supported (same limitation as `'!FAST`).

## Labels

Labels are identifiers prefixed with `.` and suffixed with `:`.

```
.loop:
    jmp .loop

.done:
    exit
```

Rules:
- Labels must be unique within the block.
- Forward and backward references are both supported (forward jumps are
  resolved in a fixup pass after the block is fully parsed).
- Label names are case-insensitive.

## Instructions

Instructions use a three-address format: `mnemonic dst, src1, src2`.

Comments start with `;` and extend to end of line.

### Integer arithmetic — `[op] dst, src1, src2` (4 bytes)

| Mnemonic | ROP | Operation |
|----------|-----|-----------|
| `addi`   | `ROP_ADD_I` | `dst = src1 + src2` |
| `subi`   | `ROP_SUB_I` | `dst = src1 - src2` |
| `muli`   | `ROP_MUL_I` | `dst = src1 * src2` |
| `divi`   | `ROP_IDIV_I` | `dst = src1 \ src2` (integer division, error if src2=0) |
| `modi`   | `ROP_MOD_I` | `dst = src1 MOD src2` (error if src2=0) |

### Float arithmetic — `[op] dst, src1, src2` (4 bytes)

| Mnemonic | ROP | Operation |
|----------|-----|-----------|
| `addf`   | `ROP_ADD_F` | `dst = src1 + src2` |
| `subf`   | `ROP_SUB_F` | `dst = src1 - src2` |
| `mulf`   | `ROP_MUL_F` | `dst = src1 * src2` |
| `divf`   | `ROP_DIV_F` | `dst = src1 / src2` (error if src2=0.0) |

### Unary — `[op] dst, src` (3 bytes)

| Mnemonic | ROP | Operation |
|----------|-----|-----------|
| `negi`   | `ROP_NEG_I` | `dst = -src` (integer) |
| `negf`   | `ROP_NEG_F` | `dst = -src` (float) |
| `not`    | `ROP_NOT`   | `dst = !src` (logical not: 0→1, nonzero→0) |
| `inv`    | `ROP_INV`   | `dst = ~src` (bitwise invert) |

### Bitwise — `[op] dst, src1, src2` (4 bytes)

| Mnemonic | ROP | Operation |
|----------|-----|-----------|
| `and`    | `ROP_AND` | `dst = src1 AND src2` |
| `or`     | `ROP_OR`  | `dst = src1 OR src2` |
| `xor`    | `ROP_XOR` | `dst = src1 XOR src2` |
| `shl`    | `ROP_SHL` | `dst = src1 << src2` |
| `shr`    | `ROP_SHR` | `dst = src1 >> src2` (arithmetic) |

### Move / convert — `[op] dst, src` (3 bytes)

| Mnemonic | ROP | Operation |
|----------|-----|-----------|
| `mov`    | `ROP_MOV`     | `dst = src` (copy register) |
| `cvtif`  | `ROP_CVT_I2F` | `dst = FLOAT(src)` (int → float) |
| `cvtfi`  | `ROP_CVT_F2I` | `dst = INT(src)` (float → int, truncation) |

### Fused fixed-point — variable size

| Mnemonic | ROP | Format | Operation |
|----------|-----|--------|-----------|
| `sqrshr`     | `ROP_SQRSHR`     | `dst, a, bits` (4 bytes) | `dst = (a*a) >> bits` |
| `mulshr`     | `ROP_MULSHR`     | `dst, a, b, bits` (5 bytes) | `dst = (a*b) >> bits` |
| `mulshradd`  | `ROP_MULSHRADD`  | `dst, a, b, bits, c` (6 bytes) | `dst = (a*b)>>bits + c` |

The `bits` operand is a register (typically holding a constant).
These use 128-bit intermediate multiplication internally — no overflow
for any 64-bit inputs.

### Integer comparison — `[op] dst, src1, src2` (4 bytes)

| Mnemonic | ROP | Operation |
|----------|-----|-----------|
| `eqi`    | `ROP_EQ_I` | `dst = (src1 == src2) ? 1 : 0` |
| `nei`    | `ROP_NE_I` | `dst = (src1 != src2) ? 1 : 0` |
| `lti`    | `ROP_LT_I` | `dst = (src1 < src2) ? 1 : 0` |
| `gti`    | `ROP_GT_I` | `dst = (src1 > src2) ? 1 : 0` |
| `lei`    | `ROP_LE_I` | `dst = (src1 <= src2) ? 1 : 0` |
| `gei`    | `ROP_GE_I` | `dst = (src1 >= src2) ? 1 : 0` |

### Fused compare-and-jump (integer) — `[op] src1, src2, label` (5 bytes)

| Mnemonic | ROP | Jumps when |
|----------|-----|-----------|
| `jeq`    | `ROP_JCMP_EQ_I` | `src1 == src2` |
| `jne`    | `ROP_JCMP_NE_I` | `src1 != src2` |
| `jlt`    | `ROP_JCMP_LT_I` | `src1 < src2` |
| `jgt`    | `ROP_JCMP_GT_I` | `src1 > src2` |
| `jle`    | `ROP_JCMP_LE_I` | `src1 <= src2` |
| `jge`    | `ROP_JCMP_GE_I` | `src1 >= src2` |

### Conditional jump on zero/nonzero — `[op] src, label` (4 bytes)

| Mnemonic | ROP | Jumps when |
|----------|-----|-----------|
| `jz`     | `ROP_JZ`  | `src == 0` |
| `jnz`    | `ROP_JNZ` | `src != 0` |

### 1D Array access — `[op] reg, array, idx_reg` (4 bytes)

| Mnemonic | ROP | Operation |
|----------|-----|-----------|
| `loadi.a`  | `ROP_LOAD_ARR_I`  | `reg = array[idx_reg]` (integer) |
| `storei.a` | `ROP_STORE_ARR_I` | `array[idx_reg] = reg` (integer) |
| `loadf.a`  | `ROP_LOAD_ARR_F`  | `reg = array[idx_reg]` (float) |
| `storef.a` | `ROP_STORE_ARR_F` | `array[idx_reg] = reg` (float) |

The `array` operand is the bare name of a `.array`-declared array.
The `idx_reg` operand is a register (variable or constant name).
Bounds-checked at runtime.

```
.array buf%()
    loadi.a  temp, buf, i    ; temp = buf%(i%)
    storei.a temp, buf, i    ; buf%(i%) = temp
```

### Control flow

| Mnemonic | ROP | Format | Operation |
|----------|-----|--------|-----------|
| `jmp`      | `ROP_JMP`      | `label` (3 bytes) | unconditional jump |
| `exit`     | `ROP_EXIT`     | (1 byte) | exit the ASM block |
| `checkint` | `ROP_CHECKINT` | (1 byte) | poll CTRL-C |

## Encoding

The assembler emits identical binary output to the `'!FAST` auto-converter:

```
[OP_FAST_LOOP:8] [total_len:16]
[nregs:8] [nlocals:8] [nglobals:0] [nconsts:8] [narrays:8]
[array_map:  narrays  × (is_local:8 slot:16)]
[const_data: nconsts  × (type:8 value:64)]
[micro_ops:  assembled instructions ... ROP_EXIT]
```

`nglobals` is always 0 — `'!ASM` only supports local variables.  Global
variables are not accessible from ASM blocks; use a local parameter to
pass values in and out.

The assembler always appends an implicit `ROP_EXIT` at the end of the
micro-op stream (same as the `'!FAST` auto-converter), so execution
cannot fall off the end of the block even if the user omits `exit`.

## Register file layout

```
regs[0 .. nlocals-1]                  = local variables (auto-mapped from frame)
regs[nlocals .. nlocals+nconsts-1]    = constants (.const + auto-interned literals)
```

Total must not exceed 64 (`MAX_FAST_REGS`).

## Writeback

On block exit, the runtime writes back `regs[0..nlocals-1]` to local frame
slots unconditionally.  Constants are discarded.

## Error reporting

Assembler errors include the physical line number of the `.bas` source
file where the error occurred.  The assembler tracks line numbers as it
consumes lines between `'!ASM` and `'!ENDASM`.  Errors are reported
through the standard `bc_set_error` mechanism with the line number set
to the offending assembly line.

## Restrictions

- **No strings.** All registers hold `int64_t` (reinterpreted as `double`
  for float operations via `memcpy`).
- **No function calls.** The micro-op engine has no CALL instruction.
- **No float comparisons.** `jeq`/`jlt`/etc. are integer-only.  Convert
  floats to integers first with `cvtfi`, or compare fixed-point integers
  directly (which is the whole point of fixed-point).
- **No globals.** Only local variables and parameters are mapped to
  registers.  Pass globals in via SUB/FUNCTION parameters.
- **1D arrays only.** Multi-dimensional arrays are not supported.
- **Must be inside SUB/FUNCTION.** Module-scope code cannot use `'!ASM`.
- **64-register limit.** Total of locals + constants must not exceed
  `MAX_FAST_REGS` (64).
- **No automatic `checkint`.** If you want CTRL-C to work, add `checkint`
  yourself.  An infinite loop without it will require a hard reset.
- **No type safety.** The assembler does not track whether a register
  holds an integer or float.  Using `addi` on a float register or `addf`
  on an integer register will produce garbage.  The programmer is
  responsible for type correctness.

## Implementation notes

### Multi-line consumption

The main compile loop in `bc_compile_source` calls `source_compile_line`
with a copied line buffer, one logical line at a time.  The `'!ASM`
block must consume multiple lines.

Implementation: when `source_compile_line` detects `'!ASM`, it sets a
flag on `BCSourceFrontend` (e.g. `asm_active`).  Subsequent calls to
`source_compile_line` accumulate lines into a buffer on the frontend
state.  When `'!ENDASM` is seen, the assembled block is emitted and the
flag is cleared.  This mirrors the existing `fast_next_loop` flag pattern
but accumulates content instead of just setting a boolean.

### Predeclaration pass

The `source_predeclare_subfuns` pass scans all source lines looking for
`SUB`/`FUNCTION` declarations.  Assembly lines between `'!ASM` and
`'!ENDASM` are not BASIC and must be skipped.  The predeclare pass
should track an `in_asm` flag and skip lines while set, to avoid
misinterpreting assembly mnemonics as BASIC keywords.

### ROP_LOAD_IMM_I / ROP_LOAD_IMM_F

The VM executor supports `ROP_LOAD_IMM_I` (opcode 35) and
`ROP_LOAD_IMM_F` (opcode 36) — 10-byte instructions that load a 64-bit
immediate directly into a register.  The assembler does not expose these.
All constants are loaded via the header constant pool, which is more
compact for values used multiple times and avoids 10-byte instructions
in the inner loop.  This is a deliberate omission, not a gap.

## Example: Fixed-point Mandelbrot inner loop

```basic
CONST FP_BITS% = 20
CONST FP_SCALE% = 1048576
CONST FP_FOUR% = 4194304

FUNCTION MandelbrotFP%(fpcx%, fpcy%, max_iter%)
  LOCAL zr%, zi%, zr2%, zi2%, mag%, i%

  '!ASM
  .const BITS,    20
  .const FOUR_FP, 4194304
  .const ZERO,    0
  .const ONE,     1
  .const SHIFT1,  19

      mov        zr, ZERO
      mov        zi, ZERO
      mov        i,  ZERO
  .loop:
      sqrshr     zr2, zr, BITS
      sqrshr     zi2, zi, BITS
      addi       mag, zr2, zi2
      jge        mag, FOUR_FP, .done
      jge        i, max_iter, .done
      mulshradd  zi, zr, zi, SHIFT1, fpcy
      subi       zr, zr2, zi2
      addi       zr, zr, fpcx
      addi       i, i, ONE
      checkint
      jmp        .loop
  .done:
      exit
  '!ENDASM

  MandelbrotFP% = i%
END FUNCTION
```
