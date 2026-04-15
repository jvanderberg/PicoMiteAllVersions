# Bridge Restoration Plan

Restore the interpreter as the primary runtime, with the VM as a performance backend behind `FRUN`. The VM bridges back to the interpreter for commands it doesn't have native opcodes for.

## Architecture

- **Interpreter** owns: prompt, REPL, shell commands, all built-in commands/functions
- **VM** owns: bytecode execution for `FRUN` programs — native opcodes for hot paths (arithmetic, control flow, arrays, graphics)
- **Bridge** handles: commands/functions the VM doesn't have native opcodes for — pre-tokenized at compile time, dispatched to interpreter handlers at runtime
- **Shared memory**: VM and interpreter use the same heap (`GetMemory`/`FreeMemory`), same `g_vartbl`, same file handles

## Steps

### 1. Restore PicoMite.c main loop to stock interpreter prompt

Revert the `bc_run_immediate()` changes in `PicoMite.c`. The main loop goes back to `tokenise() → ExecuteProgram()` for all prompt input. The interpreter owns the REPL again — no `bc_run_immediate`, no `bc_try_compile_line`.

Status: pending

### 2. Bring back FRUN command

Restore `cmd_frun()` as the VM entry point. Called from the interpreter prompt like any command. Flow:

1. Interpreter tokenizes and dispatches `FRUN` normally
2. `cmd_frun()` reads source from ProgMemory (already loaded by interpreter)
3. Compiles via `bc_source.c` to bytecode
4. Saves `mark` jmp_buf
5. Runs `bc_vm_execute()`
6. Restores `mark`, cleans up

Adapted from the `cmd_frun()` at commit `2112876`, but using `bc_source.c` instead of the old tokenized compiler.

Status: pending

### 3. Switch VM allocations to interpreter's allocator

Replace `bc_alloc.c` arena usage with `GetMemory`/`FreeMemory` for:

- Compiler scratch (tables, metadata)
- VM runtime arrays (`OP_DIM_ARR` allocations)
- String storage
- `BCCompiler` and `BCVMState` structs themselves

This is the biggest mechanical change. The `bc_alloc.c` arena was introduced to isolate VM memory from the interpreter heap. Going back to shared memory means:

- Variable sync works naturally (both sides see the same heap)
- No separate arena to manage
- `GetTempMemory` available for bridge calls
- Must ensure VM cleanup frees everything it allocated

`bc_alloc.c` can remain as a thin wrapper (`BC_ALLOC` → `GetMemory`, `BC_FREE` → `FreeMemory`) or be removed entirely.

Status: pending

### 4. Pre-tokenize bridged calls in bc_source.c

When `bc_source.c` encounters a command/function it can't compile natively:

1. Instead of `"Unsupported VM command"` error, capture the raw source text of that statement
2. Call `tokenise()` on it at compile time
3. Emit `OP_BRIDGE_CMD` (or `OP_BRIDGE_FUN`) followed by the tokenized bytes in the bytecode stream

At runtime the VM reads the pre-tokenized bytes and hands them directly to the interpreter handler. Zero runtime tokenization cost.

The compiler already has the decision logic — it's the same code path that currently errors. This is changing the error into an emit.

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
- `vm_device_support.h` — VM-only legacy import seam
- `bc_run_immediate()` / `bc_try_compile_line()` — REPL functions (interpreter handles REPL)
- `PICOMITE_VM_DEVICE_ONLY` build target and all `#ifdef` guards for it
- Possibly `bc_alloc.c` if we fully revert to interpreter allocator

## What gets modified

- `PicoMite.c` — revert REPL to stock, add `FRUN` to command table
- `bc_source.c` — unsupported commands emit `OP_BRIDGE_CMD` instead of error
- `bc_vm.c` — add `OP_BRIDGE_CMD`/`OP_BRIDGE_FUN` dispatch to bridge functions
- `bytecode.h` — add bridge opcodes, remove VM-only shell definitions
- `bc_runtime.c` — simplify to `cmd_frun` entry point
- `CMakeLists.txt` / `CMakeLists 2350.txt` — remove VM-only target, add `bc_bridge.c`
- `AllCommands.h` — restore `FRUN` command declaration

## Git references

- **Current branch:** `fastgfx` — all current work is here
- **Last commit before this work:** `2c8f64a` — REPL immediate mode, vm_sys_file fixes, doc cleanup
- **Bridge source code:** `git show 2112876:bc_bridge.c` — 663-line bridge with variable sync, cmd/fun dispatch, cmd_frun
- **Bridge removal commit:** `91ce021` — "Remove VM bridge path" — this is what we're partially reverting
- **Old tokenized compiler:** `git show 2112876:bc_compiler.c` (NOT needed — we keep `bc_source.c`)
- **Old cmd_frun with bridge:** `git show 2112876:bc_bridge.c | tail -200` — the FRUN entry point
- **Stock PicoMite.c main loop:** check `git log --oneline PicoMite.c` for the last commit before `bc_run_immediate` was added; the `autorun:` label and `ExecuteProgram(tknbuf)` path is what needs restoring

## Key files to read first

- `bc_source.c` — the current compiler (keep this, modify to emit bridge ops)
- `bc_vm.c` — the current VM dispatch (keep this, add bridge op handlers)
- `bc_runtime.c` — current VM entry points (simplify to cmd_frun)
- `bc_alloc.c` — current arena allocator (replace with interpreter allocator)
- `bytecode.h` — opcode/syscall definitions (add OP_BRIDGE_CMD/OP_BRIDGE_FUN)
- `PicoMite.c:4650-4780` — the main loop that was modified to use bc_run_immediate
- `AllCommands.h` — command table declarations

## Risk / notes

- The variable sync layer is the most delicate part. It must handle: scalars (int, float, string), arrays (all types), locals inside SUB/FUNCTION, and the function return value slot. The `2112876` version handled all of these.
- `longjmp(mark)` from interpreter error handlers must be in the bridge's save/restore chain. The `2112876` version handled this.
- Host build needs a path that doesn't depend on `GetMemory` — keep `#ifdef MMBASIC_HOST` using `calloc`/`free` as before.
- Test strategy: existing host oracle tests (`run_tests.sh`) should keep working. The VM path just needs `FRUN` instead of `RUN` as the entry point.
