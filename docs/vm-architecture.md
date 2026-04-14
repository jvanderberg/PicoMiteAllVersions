# VM Architecture Snapshot

This document describes the current VM-oriented prototype architecture as it exists in the tree today.

## Scope

- BASIC program execution on device is VM-owned.
- The host contains both:
  - the legacy interpreter oracle
  - the VM implementation and test harness
- The default firmware path still carries legacy prompt/control infrastructure.
- The dedicated `PICOMITE_VM_DEVICE_ONLY` target now has a VM-owned shell/control layer instead of the legacy prompt.

## High-Level Shape

### Host

- `host/mmbasic_test --interp`
  - runs the legacy interpreter path
  - uses `host/host_stubs_legacy.c` as the host hardware/environment shim boundary
- `host/mmbasic_test --vm`
  - runs the VM-owned source frontend and VM runtime
- default compare mode
  - runs interpreter first, then VM
  - compares stdout and error behavior
- pixel tests
  - compare host-rendered framebuffers where the host oracle supports the primitive

### Device

- `RUN`
  - loads source
  - compiles through the VM-owned source frontend
  - executes bytecode on the VM
- prompt/shell
  - default firmware still uses selected legacy prompt infrastructure
  - `PICOMITE_VM_DEVICE_ONLY` uses `vm_device_main.c` and `vm_device_fileio.c`
  - immediate BASIC is disabled in both

## Major Components

### Legacy Oracle Path

- Core sources:
  - `MMBasic.c`
  - `Commands.c`
  - `Functions.c`
  - `Operators.c`
  - `MATHS.c`
  - `Memory.c`
- Host boundary:
  - `host/host_stubs_legacy.c`
- Purpose:
  - semantic oracle for language behavior
  - oracle for host-safe syscalls where the host shim is independent

Current caveat:
- oracle independence is not complete yet
- some host legacy shim paths still route through `vm_sys_*`
- until that is removed, those families are weaker oracle coverage than pure interpreter-only paths

### VM Frontend and Runtime

- Source frontend:
  - `bc_source.c`
- Compiler/core metadata:
  - `bc_compiler_core.c`
  - `bytecode.h`
- VM execution:
  - `bc_vm.c`
  - `bc_runtime.c`
- Debug/disassembly:
  - `bc_debug.c`

The VM frontend compiles raw `.bas` source directly. It does not depend on the legacy tokenizer for the VM path.

## VM Execution Model

### Core VM Ops

These remain dedicated VM instructions:

- stack/value movement
- variable and array load/store
- type conversion
- arithmetic, comparisons, and bitwise ops
- control flow
- call/frame management
- DATA/READ/RESTORE
- hot language/runtime builtins

These are language/runtime semantics, not syscalls.

### Syscall ABI

The default syscall path is now a generic ABI:

- `OP_SYSCALL`
- syscall/intrinsic id
- standard decode/dispatch path in `bc_vm.c`

Current implemented syscall families already emitted through the generic ABI by default:

- graphics and graphics-query builtins
- `FRAMEBUFFER` (CREATE, LAYER, WRITE, CLOSE, MERGE, SYNC, WAIT, COPY)
- `FASTGFX`
- `DATE$`, `TIME$`, `KEYDOWN()`, `PAUSE`
- `PLAY`
- `SETPIN`, `PIN()`, `PWM`, `SERVO`
- current file commands and file I/O operations

Dedicated opcodes still exist for core VM semantics and may remain for proven hot paths, but they are no longer the default expansion pattern for syscalls.

### Peephole Optimizer

The source frontend includes a compile-time peephole optimizer controlled by `bc_opt_level` (default: 1, CLI: `-O0`/`-O1`).

The optimizer rewrites common instruction sequences into fused superinstructions during compilation. It operates on a 1-2 instruction peephole window immediately after parsing.

#### Implemented superinstructions

| Opcode | Pattern | Effect |
|--------|---------|--------|
| `OP_MATH_MULSHR` | `(a * b) \ 2^n` | Fused multiply + integer shift-right |
| `OP_MATH_SQRSHR` | `(a * a) \ 2^n` | Same-variable squared variant |
| `OP_MATH_MULSHRADD` | `((a * b) \ 2^n) + c` | MULSHR cascaded with addition |
| `OP_JCMP_I` | integer compare + conditional jump | Fused comparison + branch |
| `OP_JCMP_F` | float compare + conditional jump | Float variant of JCMP |
| `OP_MOV_VAR` | `a = b` (simple assignment) | Direct typed variable copy |

Additional peephole rewrites:
- `INC a%, const` — constant increment folded into assignment
- `INC a%, expr` / `INC a, expr` — expression increment folded for integer and float

#### Testing methodology

Peephole optimizations are tested in paired fashion:

- **Peephole tests** (`t0XX_*_peephole.bas`): run with `--vm-disasm` and verify the expected fused opcode appears (and the unfused form does not).
- **Equivalence tests** (`t0XX_*_opt_equiv.bas`): compare `-O0` output vs `-O1` output byte-for-byte to verify behavioral correctness.

A mandelbrot smoke test runs the full program with `-O1` as an integration check.

Implementation lives in `bc_source.c` (fusion functions: `source_try_fuse_mulshr()`, `source_try_fuse_mulshradd()`, `source_try_fuse_mov_assignment()`, `source_jcmp_relation()`, `source_emit_rel_jump()`). Opcodes are defined in `bytecode.h`.

## Native Syscall Rule

VM syscall behavior must live in VM-owned code.

Rules:

- **This is a blocking repository rule, not a preference.**
- copy/adapt useful logic from legacy sources
- copy/adapt dependent legacy helpers when the legacy implementation depends on them
- do not call or wrap legacy `cmd_*`, `fun_*`, `Ext*`, or old drawing/file handlers from the VM path
- do not invent new algorithms when legacy behavior already exists
- keep novel code limited to:
  - frontend/dispatch glue
  - required adaptation layers
  - host mocks/shims

Current VM syscall modules:

- `vm_sys_graphics.c`
- `vm_sys_file.c`
- `vm_sys_pin.c`
- `vm_sys_audio.c`
- `vm_sys_time.c`
- `vm_sys_input.c`

## Memory Model

The allocator is no longer fully monolithic, but it is not fully separated yet.

### Current State

- device heap:
  - `bc_alloc.c`
  - fixed `bc_heap` arena on device
- compile scratch:
  - temporary compile arena carved from the top of the device heap
  - released wholesale before execution
- program image/runtime:
  - retained compiler output is copied into runtime-owned storage before the compile arena is released
- VM instance state:
  - `BCCompiler` and `BCVMState` are explicit device-static state, not heap-allocated on device
- graphics scratch:
  - reusable grow-on-demand scratch buffers live in `vm_sys_graphics.c`
  - reset between runs with `vm_sys_graphics_reset()`

### What This Fixes

- compile-time scratch no longer competes with runtime allocations during program execution
- hot graphics paths no longer churn heap allocations on every triangle/polygon/arc/thick-circle draw

### What Still Shares the Runtime Heap

- BASIC arrays
- mutable strings
- runtime metadata/tables
- non-graphics syscall scratch that has not yet been isolated

So memory separation is improved, but not complete.

## Graphics Architecture

Graphics on the VM path are native VM syscalls, not interpreter bridge calls.

Implemented primitive families:

- `BOX`
- `RBOX`
- `ARC`
- `TRIANGLE`
- `POLYGON`
- `CIRCLE`
- `LINE`
- `PIXEL`
- `TEXT`
- `FONT`
- `COLOUR` / `COLOR`
- `CLS`
- `FASTGFX`
- `FRAMEBUFFER` (CREATE, LAYER, WRITE, CLOSE, MERGE, SYNC, WAIT, COPY)

The `FRAMEBUFFER` implementation covers the LCD-style dual-buffer model: `CREATE` allocates a frame buffer, `LAYER [colour]` allocates a layer buffer with optional transparent color, `WRITE N/F/L` switches the drawing target, `MERGE` composites the layer onto the frame (NOW/B/R/A modes), `COPY` transfers between buffers with optional background mode, and `SYNC`/`WAIT` handle synchronization.

The host framebuffer backend (`host/host_framebuffer_backend.h`) provides a full 32-bit-per-pixel simulation of the dual-buffer model for deterministic oracle comparison.

Host framebuffer comparison is useful for deterministic regressions, but device validation is still required for:

- LCD clearing
- frame pacing
- FASTGFX/device presentation behavior
- FRAMEBUFFER merge timing and background copy behavior on real hardware
- SD and memory-pressure behavior

## Prompt / Shell Boundary

The prompt boundary now depends on which device target is being discussed.

Current state:

- BASIC program execution is VM-owned
- the default firmware still depends on selected legacy prompt infrastructure
- the `PICOMITE_VM_DEVICE_ONLY` build has a VM-owned command-table shell
  - current shell commands implemented there:
    - `RUN`
    - bare filename run
    - `LOAD`
    - `SAVE`
    - `LIST`
    - `NEW`
    - `FILES`
    - `DRIVE`
    - `CHDIR` / `CD`
    - `MKDIR`
    - `RMDIR`
    - `KILL`
    - `COPY`
    - `RENAME`
    - `HELP`
    - `MEMORY`
    - `FREE`
    - `PWD`
    - `CLS`
- shell commands still pending in the VM-only build:
  - `EDIT`
  - `AUTOSAVE`
  - `OPTION`
  - `CONFIGURE`
  - full prompt parity for submodes like `FILES ... ,TIME` and the broader `LIST`/`OPTION` families

## Verification Status

Current baseline at the time of this document update:

- `./host/run_tests.sh`: `168 passed, 0 failed`
- `./host/run_frontend_tests.sh`: `48 passed, 0 failed`
- `./host/run_optimizer_tests.sh`: `22 passed, 0 failed`
- `./host/run_pixel_tests.sh`: passed
- `./host/run_host_shim_tests.sh`: `4 passed, 0 failed`
- `bash ./host/run_unsupported_tests.sh`: `0 passed, 0 failed`
- `./host/run_missing_syscall_tests.sh`: `0 passed, 0 failed`
- `make -C build2350 -j8`: passed

## Remaining Architectural Risks

Highest-priority current risks:

1. Oracle independence is incomplete.
2. Non-graphics syscall scratch still shares the runtime heap.
3. The generic syscall ABI exists, but obsolete dedicated syscall paths still remain in the runtime and need cleanup.

## Reference Documents

- [VM Cutover Plan](./vm-cutover-plan.md)
- [VM Command Coverage](./vm-command-coverage.md)
- [Host Harness README](../host/README.md)
