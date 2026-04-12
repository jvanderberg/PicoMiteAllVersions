# VM Architecture Snapshot

This document describes the current VM-oriented prototype architecture as it exists in the tree today.

## Scope

- BASIC program execution on device is VM-owned.
- The host contains both:
  - the legacy interpreter oracle
  - the VM implementation and test harness
- The device prompt is still legacy-owned shell/control code, not a VM REPL.

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
  - still uses selected legacy command infrastructure for OS/file/editor behavior
  - immediate BASIC is disabled

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
- `FASTGFX`
- `DATE$`, `TIME$`, `KEYDOWN()`, `PAUSE`
- `PLAY`
- `SETPIN`, `PIN()`, `PWM`, `SERVO`
- current file commands and file I/O operations

Dedicated opcodes still exist for core VM semantics and may remain for proven hot paths, but they are no longer the default expansion pattern for syscalls.

## Native Syscall Rule

VM syscall behavior must live in VM-owned code.

Rules:

- copy/adapt useful logic from legacy sources
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

Host framebuffer comparison is useful for deterministic regressions, but device validation is still required for:

- LCD clearing
- frame pacing
- FASTGFX/device presentation behavior
- SD and memory-pressure behavior

## Prompt / Shell Boundary

The current device firmware is not yet a fully isolated VM environment internally.

Current state:

- BASIC program execution is VM-owned
- prompt and shell behavior still depend on selected legacy infrastructure
- this is intentional for the current prototype, but it means the device image still carries some legacy code for command tables, editing, and shell-adjacent behavior

## Verification Status

Current baseline at the time of this document update:

- `./host/run_tests.sh`: `162 passed, 0 failed`
- `./host/run_frontend_tests.sh`: `27 passed, 0 failed`
- `./host/run_pixel_tests.sh`: passed
- `./host/run_host_shim_tests.sh`: `4 passed, 0 failed`
- `bash ./host/run_unsupported_tests.sh`: `0 passed, 0 failed`
- `./host/run_missing_syscall_tests.sh`: `0 passed, 0 failed`
- `make -C build2350 -j8`: passed
- `arm-none-eabi-size build2350/PicoMite.elf`: `text=976656`, `bss=296140`

## Remaining Architectural Risks

Highest-priority current risks:

1. Oracle independence is incomplete.
2. Non-graphics syscall scratch still shares the runtime heap.
3. The generic syscall ABI exists, but obsolete dedicated syscall paths still remain in the runtime and need cleanup.

## Reference Documents

- [VM Cutover Plan](./vm-cutover-plan.md)
- [VM Command Coverage](./vm-command-coverage.md)
- [Host Harness README](../host/README.md)
