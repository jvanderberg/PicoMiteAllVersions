# Bridge Restoration Plan

Restore the interpreter as the primary runtime, with the VM as a performance backend behind `FRUN`. The VM bridges back to the interpreter for commands it doesn't have native opcodes for.

## Architecture

- **Interpreter** owns: prompt, REPL, shell commands, all built-in commands/functions
- **VM** owns: bytecode execution for `FRUN` programs — native opcodes for hot paths (arithmetic, control flow, arrays, graphics)
- **Bridge** handles: commands/functions the VM doesn't have native opcodes for — pre-tokenized at compile time, dispatched to interpreter handlers at runtime
- **Shared memory**: VM and interpreter use the same heap (`GetMemory`/`FreeMemory`), same `g_vartbl`, same file handles

## Build modes

There are exactly two build modes:

- **Host** (`MMBASIC_HOST`): macOS native build for off-device testing. Uses `calloc`/`free`. Built via `host/Makefile`.
- **Device**: RP2040/RP2350 firmware. Interpreter + VM behind FRUN, sharing the interpreter's page-based MMHeap via `TryGetMemory`/`FreeMemory`. Built via `CMakeLists.txt`.

There is no separate "VM-only device" build. `PICOMITE_VM_DEVICE_ONLY` and its associated code are dead and will be removed.

## Validation requirements

Every step that touches the VM, call structure, or memory management must pass the host build gate before proceeding:

1. **Host build compiles clean:** `cd host/ && ./build.sh` — zero warnings, zero errors.
2. **Full test suite passes:** `cd host/ && ./run_tests.sh` — all tests PASS in the default comparison mode (interpreter oracle vs VM).
3. **Interpreter-only mode works:** `./run_tests.sh --interp` — interpreter path is not broken.
4. **VM-only mode works:** `./run_tests.sh --vm` — VM path is not broken.

The default comparison mode is the most important gate — it runs the same BASIC program through both the interpreter and the VM, then asserts output equality. Any mismatch is a failure.

**The host build must be preserved throughout.** `host_main.c` is the test driver and is NOT touched by PicoMite.c changes (device build only). But any changes to `bc_runtime.c`, `bc_vm.c`, `bc_source.c`, `bc_alloc.c`, `bytecode.h`, or the interpreter core (`MMBasic.c`, `Commands.c`, etc.) affect the host build and must be validated.

The `--immediate` and `--try-compile` modes in host_main.c use `bc_run_immediate()` and `bc_try_compile_line()`. These modes may be removed or adapted in step 2 when we simplify `bc_runtime.c` to the `cmd_frun` entry point, but the core comparison mode must always work.

## Steps

### 1. Restore PicoMite.c main loop to stock interpreter prompt

Revert the `bc_run_immediate()` changes in `PicoMite.c`. The main loop goes back to `tokenise() → ExecuteProgram()` for all prompt input. The interpreter owns the REPL again — no `bc_run_immediate`, no `bc_try_compile_line`.

**Scope:** Device build only (`PicoMite.c` is not in the host Makefile). No host validation needed for this step alone, but confirm the host build still compiles (no header/extern drift).

**Validation:** `cd host/ && ./build.sh && ./run_tests.sh` — must still pass (this step shouldn't affect host, but verify).

Status: done

### 2. Bring back FRUN command

Restore `cmd_frun()` as the VM entry point. Called from the interpreter prompt like any command. Flow:

1. Interpreter tokenizes and dispatches `FRUN` normally
2. `cmd_frun()` reads source from ProgMemory (already loaded by interpreter)
3. Compiles via `bc_source.c` to bytecode
4. Saves `mark` jmp_buf
5. Runs `bc_vm_execute()`
6. Restores `mark`, cleans up

Adapted from the `cmd_frun()` at commit `2112876`, but using `bc_source.c` instead of the old tokenized compiler.

**Host impact:** `bc_runtime.c` is in the host build. The `--immediate` and `--try-compile` modes in `host_main.c` call `bc_run_immediate()` / `bc_try_compile_line()`. These must either be preserved (keep the functions in `bc_runtime.c` alongside `cmd_frun`) or the host driver must be updated to use the new entry point. The default comparison mode calls `bc_run_source_string()` which must remain working.

**Validation:** Full host gate — `./build.sh && ./run_tests.sh` (default comparison mode). Also `./run_tests.sh --vm` and `./run_tests.sh --interp` individually.

Status: done

### 3. Switch VM allocations to interpreter's allocator

Replaced the 200+ line arena allocator in `bc_alloc.c` with a two-path design:

- **Host** (`MMBASIC_HOST`): `calloc`/`free` — unchanged.
- **Device** (`#else`): thin wrappers around `TryGetMemory`/`FreeMemory`, sharing the interpreter's page-based MMHeap.

The old `PICOMITE_VM_DEVICE_ONLY` arena block was removed entirely. `bc_alloc.c` is now ~100 lines. `BCCompiler` and `BCVMState` are heap-allocated via `BC_ALLOC`/`BC_FREE` on all platforms (no more statics on device). `TryGetMemory` added to `Memory.c` — same page allocator as `GetMemory` but returns NULL on OOM.

Validated with ASAN. 180/180 tests pass.

Status: done

### 4. Pre-tokenize bridged calls in bc_source.c

When `bc_source.c` encounters a command/function it can't compile natively:

1. Instead of `"Unsupported VM command"` error, capture the raw source text of that statement
2. Call `tokenise()` on it at compile time
3. Emit `OP_BRIDGE_CMD` (or `OP_BRIDGE_FUN`) followed by the tokenized bytes in the bytecode stream

At runtime the VM reads the pre-tokenized bytes and hands them directly to the interpreter handler. Zero runtime tokenization cost.

The compiler already has the decision logic — it's the same code path that currently errors. This is changing the error into an emit.

**Host impact:** `bc_source.c` is in the host build. Changes to the compiler affect VM compilation for all test programs. Programs that previously errored with "Unsupported VM command" will now emit bridge ops instead — but those bridge ops must be handled by the VM dispatch (step 5). Do steps 4 and 5 together, or add a stub handler that errors cleanly.

**Validation:** Full host gate — `./build.sh && ./run_tests.sh`. Existing tests that only use natively-compiled commands should still pass unchanged. New tests can be added for bridged commands.

Status: pending

### 5. Restore bridge dispatch

Bring back `bc_bridge_call_cmd()` and `bc_bridge_call_fun()` adapted from commit `2112876`. Core logic:

**Command bridge:**
1. `sync_vm_to_mmbasic()` — copy VM globals → `g_vartbl`
2. Set up `cmdtoken`, `cmdline`, `nextstmt` from pre-tokenized bytes
3. Call `commandtbl[idx].fptr()`
4. `sync_mmbasic_to_vm()` — copy `g_vartbl` → VM globals
5. `ClearTempMemory()`

**Function bridge:**
1. `sync_vm_to_mmbasic()`
2. Set up `ep`, `targ` from pre-tokenized bytes
3. Call `tokentbl[idx].fptr()`
4. Push `iret`/`fret`/`sret` to VM stack
5. `sync_mmbasic_to_vm()`
6. `ClearTempMemory()`

**Variable sync** maps VM slot indices to `g_vartbl` indices. Cached mapping built lazily on first bridge call. Handles scalars, arrays, locals, and strings.

The sync layer from `2112876` should work largely as-is since we're going back to shared memory.

**Host impact:** `bc_bridge.c` will be a new file added to the host Makefile. The variable sync layer touches `g_vartbl`, `varcnt`, locals — all shared interpreter state. Must be testable in host mode.

**Validation:** Full host gate — `./build.sh && ./run_tests.sh`. Add new test programs that exercise bridged commands (commands not natively compiled) and verify output matches the interpreter oracle. Test with: string operations, array manipulation, SUB/FUNCTION with locals, and error handling (bridge call that triggers `error()`).

Status: pending

### 6. Keep all native VM advances

These are preserved unchanged:

- `bc_vm.c` — core dispatch loop, computed goto, all arithmetic/control flow/variable opcodes
- `bc_source.c` — recursive descent parser/compiler (the clean replacement for the tokenized compiler)
- Peephole optimizer — `OP_MATH_MULSHR`, `OP_JCMP_I/F`, `OP_MOV_VAR`, etc.
- `OP_SYSCALL` ABI and dispatcher
- `vm_sys_graphics.c` — native graphics syscalls (BOX, CIRCLE, LINE, TEXT, POLYGON, etc.)
- `vm_sys_file.c` — native file I/O syscalls (OPEN, CLOSE, PRINT, LINE INPUT)
- Other `vm_sys_*.c` modules that have native implementations

Native opcodes always take priority. The bridge is only the fallback for the long tail.

Status: no action needed

## What gets deleted

- `vm_device_main.c` — VM-only shell (interpreter owns the prompt)
- `vm_device_fileio.c` / `vm_device_fileio.h` — VM-only file/shell commands
- `vm_device_runtime.c` — VM-only device runtime
- `vm_device_support.h` — VM-only legacy import seam
- `bc_run_immediate()` / `bc_try_compile_line()` — REPL functions (interpreter handles REPL)
- All `PICOMITE_VM_DEVICE_ONLY` `#ifdef` guards and the CMake option — there is no VM-only build
- The 200+ line arena allocator that was in `bc_alloc.c` (already removed — replaced with thin TryGetMemory/FreeMemory wrapper)

## What gets modified

- `PicoMite.c` — revert REPL to stock, add `FRUN` to command table
- `bc_source.c` — unsupported commands emit `OP_BRIDGE_CMD` instead of error
- `bc_vm.c` — add `OP_BRIDGE_CMD`/`OP_BRIDGE_FUN` dispatch to bridge functions
- `bytecode.h` — add bridge opcodes, remove VM-only shell definitions
- `bc_runtime.c` — simplify to `cmd_frun` entry point
- `CMakeLists.txt` / `CMakeLists 2350.txt` — remove VM-only target, add `bc_bridge.c`
- `AllCommands.h` — restore `FRUN` command declaration

## Git references

- **Current branch:** `bridge-restoration` (branched from `fastgfx` at `33e755c`)
- **Parent branch:** `fastgfx` — last commit `33e755c` (Add bridge restoration plan)
- **Bridge source code:** `git show 2112876:bc_bridge.c` — 663-line bridge with variable sync, cmd/fun dispatch, cmd_frun
- **Bridge removal commit:** `91ce021` — "Remove VM bridge path" — this is what we're partially reverting
- **Old tokenized compiler:** `git show 2112876:bc_compiler.c` (NOT needed — we keep `bc_source.c`)
- **Old cmd_frun with bridge:** `git show 2112876:bc_bridge.c | tail -200` — the FRUN entry point
- **Stock PicoMite.c main loop:** check `git log --oneline PicoMite.c` for the last commit before `bc_run_immediate` was added; the `autorun:` label and `ExecuteProgram(tknbuf)` path is what needs restoring

## Key files to read first

- `bc_source.c` — the current compiler (keep this, modify to emit bridge ops)
- `bc_vm.c` — the current VM dispatch (keep this, add bridge op handlers)
- `bc_runtime.c` — current VM entry points (simplify to cmd_frun)
- `bc_alloc.c` — thin allocator wrapper (host: calloc, device: TryGetMemory)
- `bytecode.h` — opcode/syscall definitions (add OP_BRIDGE_CMD/OP_BRIDGE_FUN)
- `PicoMite.c:4650-4780` — the main loop that was modified to use bc_run_immediate
- `AllCommands.h` — command table declarations

## Risk / notes

- The variable sync layer is the most delicate part. It must handle: scalars (int, float, string), arrays (all types), locals inside SUB/FUNCTION, and the function return value slot. The `2112876` version handled all of these.
- `longjmp(mark)` from interpreter error handlers must be in the bridge's save/restore chain. The `2112876` version handled this.
- Host build needs a path that doesn't depend on `GetMemory` — keep `#ifdef MMBASIC_HOST` using `calloc`/`free` as before.
- Test strategy: existing host oracle tests (`run_tests.sh`) must keep working at every step. The default comparison mode (interpreter vs VM) is the primary gate. No step is "done" until the full test suite passes.
- The host `--immediate` and `--try-compile` modes call `bc_run_immediate()` and `bc_try_compile_line()`. These are convenience test modes. They can be kept, adapted, or removed as part of step 2 — but if removed, update `host_main.c` accordingly.
- Steps 4 and 5 (pre-tokenize + bridge dispatch) are tightly coupled. Implement together or add a stub `OP_BRIDGE_CMD` handler that cleanly errors, so existing tests still pass between the two steps.
- Every step must be independently committable with a green test suite. No "break now, fix later" across commits.
