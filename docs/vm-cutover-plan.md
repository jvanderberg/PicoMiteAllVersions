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

- Create and preserve a dedicated host target that keeps the original interpreter semantics.
- Keep the existing shared host backends for framebuffer, files, timers, input, and screenshots.
- Do not let VM/syscall refactors modify this oracle path.
- Use it as the correctness reference for language features and host-safe syscalls.

### 2. Remove interpreter execution from device firmware

- Remove general interpreter program execution from the device runtime.
- Do not ship both the interpreter runtime and the VM runtime in firmware.
- Keep only the parsing/tokenising pieces needed for shell commands, editing, and file workflows.

### 3. Replace the device prompt with a minimal shell

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

### 4. Make `RUN` VM-only

- `RUN` should always:
  - load/tokenise source
  - compile to bytecode
  - execute on the VM
- Remove `FRUN` from the user model.
- Either delete it or alias it to `RUN`.

### 5. Eliminate bridge and fallback behavior

- Any unimplemented VM syscall or statement must fault immediately.
- No bridge to the interpreter.
- No interpreter fallback on device.

### 6. Use host differential testing as the main validation loop

- `legacy host interpreter` is the semantic oracle.
- `host VM` is the implementation target.
- Compare outputs, errors, and host-rendered framebuffers where meaningful.
- Use device tests mainly for hardware-facing behavior, timing, and memory/resource validation.

### 7. Separate language correctness from hardware validation

- For language/runtime behavior and host-safe syscalls, trust the host oracle.
- For display, FASTGFX, timing, DMA, SD, and resource limits, validate on device.
- Do not let shared host approximations weaken the interpreter's role as the core semantic reference.

### 8. Future work, not part of this cutover

- A VM-based shell.
- A full VM REPL.
- Migrating prompt command execution into the VM.

## Success Criteria

- Device firmware contains one execution engine for BASIC programs: the VM.
- Device prompt no longer executes arbitrary BASIC.
- Host retains an untouched interpreter for semantic regression testing.
- Missing VM functionality fails loudly instead of silently bridging.
