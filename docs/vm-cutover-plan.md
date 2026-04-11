# VM Cutover Plan

## Goal

Move to a prototype architecture with:

1. A host-only legacy interpreter preserved as the semantic oracle.
2. A VM-only BASIC program engine on device.
3. A stripped-down device prompt that handles OS/shell tasks, not immediate BASIC.

## Principles

- The legacy host interpreter remains untouched for semantic validation.
- The device firmware does not carry both the full interpreter and the VM.
- `RUN` becomes VM-only.
- `FRUN` is removed from the user model.
- Missing VM functionality faults immediately.
- No bridge or interpreter fallback exists on device.

## Plan

### 1. Freeze a host-only legacy interpreter

Status: mostly complete.

- Create and preserve a dedicated host target that keeps the original interpreter semantics.
- Keep the existing shared host backends for framebuffer, files, timers, input, and screenshots.
- Do not let VM/syscall refactors modify this oracle path.
- Use it as the correctness reference for language features and host-safe syscalls.
- Current implementation uses `host/mmbasic_test` with `--interp` as the oracle path and `--vm` as the implementation path.
- The active host shim is `host/host_stubs_legacy.c`.

### 2. Remove interpreter execution from device firmware

Status: in progress.

- Remove general interpreter program execution from the device runtime.
- Do not ship both the interpreter runtime and the VM runtime in firmware.
- Keep only the parsing/tokenising pieces needed for shell commands, editing, and file workflows.
- `RUN` no longer executes `ProgMemory` through `ExecuteProgram()`.
- Remaining risk: the device build still carries interpreter/tokenising code for prompt, editing, command table, and oracle-adjacent runtime dependencies. This needs further size-driven separation before the firmware is truly VM-only internally.

### 3. Replace the device prompt with a minimal shell

Status: partially complete.

- Support operational commands such as:
  - `RUN`
  - `LOAD`
  - `SAVE`
  - `FILES`
  - `NEW`
  - `LIST`
  - `EDIT`
  - selected `OPTION` and system commands
- Reject arbitrary immediate BASIC at the prompt with a clear error.
- Treat the prompt as OS control, not a BASIC REPL.
- Current prompt rejects arbitrary immediate BASIC with `Immediate BASIC disabled`.
- Current prompt still uses selected legacy command handlers for shell/file/editor behavior.

### 4. Make `RUN` VM-only

Status: complete for the current prototype path.

- `RUN` should always:
  - load/tokenise source
  - compile to bytecode
  - execute on the VM
- Remove `FRUN` from the user model.
- `FRUN` has been removed from the command table rather than aliased.
- Host VM mode calls `bc_run_current_program()` directly.

### 5. Eliminate bridge and fallback behavior

Status: complete for the VM compiler/runtime path.

- Any unimplemented VM syscall or statement must fault immediately.
- No bridge to the interpreter.
- No interpreter fallback on device.
- `OP_BUILTIN_*` bridge opcodes and VM handlers have been removed.
- The old `bc_bridge.c` implementation has been removed and the remaining VM runtime entrypoints live in `bc_runtime.c`.
- Unsupported commands/functions now fail at compile time with `Unsupported VM command: ...` or `Unsupported VM function: ...`.
- Negative tests live under `host/tests/unsupported/` and are run by `host/run_unsupported_tests.sh`.

### 6. Use host differential testing as the main validation loop

Status: active.

- `legacy host interpreter` is the semantic oracle.
- `host VM` is the implementation target.
- Compare outputs, errors, and host-rendered framebuffers where meaningful.
- Oracle tests must stay within the syntax and behavior accepted by the legacy host interpreter.
- If the legacy interpreter rejects a construct, that test is not a valid oracle-comparison test until it is rewritten into a legacy-valid form.
- Use device tests mainly for hardware-facing behavior, timing, and memory/resource validation.
- Current baseline:
  - `./host/run_tests.sh`: 115 passing oracle comparison tests.
  - `./host/run_pixel_tests.sh`: passing framebuffer assertions.
  - `bash host/run_unsupported_tests.sh`: 3 passing unsupported-syscall negative tests.

### 7. Separate language correctness from hardware validation

Status: active.

- For language/runtime behavior and host-safe syscalls, trust the host oracle.
- For display, FASTGFX, timing, DMA, SD, and resource limits, validate on device.
- Do not let shared host approximations weaken the interpreter's role as the core semantic reference.
- Host framebuffer tests are useful for deterministic drawing regressions, but LCD clearing, frame pacing, SD behavior, and memory pressure remain device-validation concerns.

### 8. Future work, not part of this cutover

Status: deferred.

- A VM-based shell.
- A full VM REPL.
- Migrating prompt command execution into the VM.

## Current Build And Test Commands

```bash
make -C host
./host/run_tests.sh
./host/run_pixel_tests.sh
bash host/run_unsupported_tests.sh
make -C build2350 -j8
arm-none-eabi-size build2350/PicoMite.elf
```

Current firmware size snapshot after bridge removal:

```text
text=932680  data=0  bss=426076  dec=1358756
```

## Success Criteria

- Device firmware contains one execution engine for BASIC programs: the VM.
- Device prompt no longer executes arbitrary BASIC.
- Host retains an untouched interpreter for semantic regression testing.
- Missing VM functionality fails loudly instead of silently bridging.

## Remaining Work

- Continue removing interpreter-only runtime from the device image where it is no longer needed by shell/tokenising/editor workflows.
- Expand native VM syscall coverage only where the legacy interpreter supports the same BASIC semantics.
- Keep unsupported VM functionality as loud compile/runtime faults with regression coverage.
- Add device-specific tests for LCD clearing, FASTGFX swaps, frame pacing, SD interactions, and long-running memory pressure.
