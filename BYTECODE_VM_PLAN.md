# MMBasic Bytecode VM — Status & Plan

## Current Status (49/49 tests passing)

### What's Done

**Compiler** (bc_compiler*.c, ~170 KB code in flash via XIP):
- Two-pass compilation from tokenized ProgMemory
- Shunting-yard expression compiler with full type tracking
- All control flow: IF/ELSEIF/ELSE/ENDIF, FOR/NEXT, DO/LOOP, WHILE/LOOP, SELECT CASE
- SUB/FUNCTION with locals, recursion, GOSUB/GOTO with labels
- PRINT with semicolons, commas, formatting
- DIM (scalar + array), LET, DATA/READ/RESTORE
- INC, CONST, RANDOMIZE, ERROR, CLEAR
- Bridge fallback for all unhandled commands

**VM** (bc_vm.c, ~104 KB code in flash via XIP):
- Computed-goto dispatch (fast on ARM)
- ~148 opcodes implemented
- Stack-based with separate int/float/string value stacks
- Local variable frames, call stack, FOR stack
- Rotating string temp buffers

**Native Functions** (no bridge overhead):
- String: LEN, LEFT$, RIGHT$, MID$, UCASE$, LCASE$, INSTR (2/3-arg), CHR$, ASC, VAL, STR$, HEX$, OCT$, BIN$, SPACE$, STRING$, INKEY$
- Math: SIN, COS, TAN, ATN, ASIN, ACOS, ATAN2, SQR, LOG, EXP, ABS, SGN, INT, FIX, CINT, RAD, DEG, PI, MAX, MIN, RND
- Everything else: bridged to interpreter's cmd_*/fun_* functions

**Test Suite** (host/tests/):
- 49 compare-mode tests — all PASS (interpreter output == VM output)
- Covers: arithmetic, strings, control flow, recursion, arrays, type coercion, DATA/READ, bridge functions, edge cases, INC/CONST, RANDOMIZE/RND/SPACE$/STRING$/INKEY$

**Host Build** (host/):
- Native macOS build for off-device testing
- `./build.sh && ./run_tests.sh` — builds and runs all 49 tests

### What's Left

## Phase 1: Memory — Use Interpreter's Heap (NEXT)

**Problem:** BCCompiler struct is 344 KB with current limits. Device heap (GetMemory pool) is 128-300 KB depending on platform. But the pool is mostly empty at FRUN time.

**Solution:** Allocate compiler arrays from MMBasic's existing GetMemory() pool with reduced limits. No new allocator needed.

Reduced limits for on-device:
| Array | Current | Device | Size |
|-------|---------|--------|------|
| code[] | 64 KB | 16 KB | 16 KB |
| constants[] | 512 | 64 | 16.5 KB |
| slots[] | 512 | 128 | 8.7 KB |
| subfuns[] | 256 | 32 | 5.4 KB |
| fixups[] | 2048 | 256 | 3.6 KB |
| linemap[] | 4096 | 512 | 3 KB |
| nest_stack[] | 64 | 16 | 3.6 KB |
| data_pool[] | 1024 | 128 | 1.1 KB |
| **Total** | **344 KB** | | **~58 KB** |

Steps:
1. Change BCCompiler from inline arrays to pointers
2. Add bc_compiler_alloc()/bc_compiler_free() that use GetMemory()/FreeMemory()
3. Use full limits on host build, reduced limits on device
4. Compile → copy final bytecode to right-sized buffer → free compiler arrays
5. VM runs from the small bytecode buffer

Memory flow:
```
FRUN:
  GetMemory(~58 KB) for compiler working data
  Compile ProgMemory → bytecode
  GetMemory(bytecode_size) for final bytecode (typically 2-10 KB)
  FreeMemory(compiler data)  ← pool is clean again
  VM executes from bytecode buffer
  FreeMemory(bytecode buffer)
```

## Phase 2: Device Build & Test

1. Add bc_*.c to CMakeLists.txt (already partly done)
2. Build for RP2350 — verify fits in flash
3. Test FRUN on actual PicoCalc hardware
4. Run benchmark comparisons (Fibonacci, Sieve, matrix multiply)

## Phase 3: Performance Benchmarking

Target speedups vs interpreter:
- Fibonacci(25): >= 3x faster
- Sieve of Eratosthenes: >= 5x faster
- Matrix multiply: >= 5x faster

The VM avoids repeated tokenized-text parsing and uses direct opcode dispatch, so tight loops should see significant improvement.

## Platform Memory Summary

| | RP2040 | RP2350 |
|---|---|---|
| Total SRAM | 264 KB | 520 KB |
| Flash | 2 MB | 4 MB |
| MMBasic heap (GetMemory pool) | 128 KB | 300 KB |
| g_vartbl | 30 KB | 48 KB |
| Max variables | 512 | 768 |
| Max SUB/FUN | 256 | 512 |
| Code execution | Flash via XIP | Flash via XIP |
| Compiler code in flash | ~170 KB | ~170 KB |
| VM code in flash | ~104 KB | ~104 KB |
| Compiler working data (from heap) | ~40-58 KB | ~58 KB |

Key insight: Compiler and VM **code** lives in flash, executes via XIP, costs zero RAM. Only the compiler's working **data** needs RAM, temporarily, from the existing interpreter heap.

## Files

```
bytecode.h              — Opcode definitions, compiler/VM structs
bc_compiler.c           — Two-pass compilation orchestration
bc_compiler_core.c      — Bytecode emission helpers, slot/constant management
bc_compiler_expr.c      — Expression compiler (shunting-yard + native functions)
bc_compiler_stmt.c      — Statement compiler (control flow, PRINT, DIM, etc.)
bc_compiler_internal.h  — Internal compiler API
bc_vm.c                 — VM dispatch loop (~148 opcodes)
bc_bridge.c             — Bridge to interpreter's cmd_*/fun_* functions
bc_test.c               — Embedded test harness (FTEST command)
host/                   — Native macOS build for off-device testing
host/tests/t*.bas       — 49 test programs
```
