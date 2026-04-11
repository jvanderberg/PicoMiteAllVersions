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
- On-device `.bas` execution compiles source through a VM-owned frontend, not through the legacy interpreter tokenizer.
- Native VM syscalls must be implemented in VM-owned runtime files by copying/adapting the useful legacy implementation logic. They must not wrap, call, or dispatch through legacy interpreter syscall handlers or helper entrypoints such as `cmd_*`, `fun_*`, `ExtCfg`, `ExtSet`, `ExtInp`, `DrawCircle`, or file command handlers.
- Shared immutable tables, device option state, and hardware SDK primitives may be used by VM syscall modules where needed, but the syscall behavior itself must live in `vm_sys_*` code.
- Legacy modules such as `FileIO.c`, `Commands.c`, `Functions.c`, and old drawing command handlers remain oracle-owned unless they are only changed behind a host/device shim boundary.
- **!!! VM SYSCALL CONVERSIONS MUST COPY LEGACY IMPLEMENTATION CODE INTO VM-OWNED MODULES AND ADAPT IT THERE !!!**
- **!!! DO NOT INVENT NEW ALGORITHMS WHEN LEGACY CODE ALREADY EXISTS !!!**
- **!!! DO NOT LINK, WRAP, OR DISPATCH BACK INTO LEGACY HANDLERS OR LEGACY DRAWING/FILE HELPER ENTRYPOINTS !!!**
- **!!! NOVEL CODE SHOULD BE LIMITED TO VM FRONTEND/DISPATCH GLUE, REQUIRED ADAPTATION LAYERS, AND HOST MOCKS/SHIMS !!!**

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
- Replace legacy BASIC tokenising for `RUN` with a VM-owned source frontend.
- Keep only the prompt/file/editing pieces needed for shell commands while this transition is underway.
- `RUN` no longer executes `ProgMemory` through `ExecuteProgram()`.
- PicoCalc RP2350 no longer reserves the legacy `AllMemory` heap; the legacy allocator API is a compatibility wrapper over `bc_alloc.c`.
- VM/compiler/runtime framebuffer buffers and remaining linked shell/display/file allocation calls now use the same device allocator.
- The remaining interpreter table reservations are `g_vartbl` and `funtbl`; these persist only because interpreter-adjacent modules are still linked.
- Remaining risk: the device build still carries interpreter/tokenising code for prompt, editing, command table, and oracle-adjacent runtime dependencies. This needs further size-driven separation before the firmware is truly VM-only internally.
- Native syscall extraction proceeds only through VM-owned source frontend opcodes and VM-owned `vm_sys_*` runtime modules.

### 2B. Split VM memory by lifetime

Status: started.

- Current device allocator state:
  - `bc_alloc.c` reserves a fixed `bc_heap` arena in `.bss`; default size is 256 KiB.
  - `BC_ALLOC()` uses this arena on device and `calloc/free` on host.
  - `BCCompiler`, `BCVMState`, compiler tables, bytecode, constants, metadata, globals, local tables, BASIC arrays, and mutable string buffers all currently share this one arena.
  - `BCVMState` contains the VM operand stack, call stack, GOSUB stack, FOR stack, and temp string buffers inline, so those stacks are arena-backed only because `BCVMState` itself is arena-allocated.
- This was useful as a prototype because it removed dependence on the legacy interpreter allocator and gave deterministic reset/ownership.
- It is not the desired final memory model because compile-only scratch, immutable program image data, and runtime mutable state have different lifetimes but currently consume one always-reserved `.bss` block.
- Target memory model:
  - Fixed VM instance state is explicit static/global storage or an explicitly reserved small VM state block.
  - Compiler scratch uses a temporary compile arena and is released before execution.
  - Bytecode, constants, DATA, symbol metadata, and sub/function metadata become a compact program image.
  - Immutable program image data should live in flash or a packed program-image buffer; mutable globals/arrays/strings stay in runtime storage.
  - Runtime heap should hold BASIC arrays, mutable strings, and other dynamic runtime objects only.
  - Shrink or eliminate the fixed 256 KiB `bc_heap` after the lifetimes are separated and device/high-water measurements justify the new size.
- Completed first implementation slice:
  - `BCCompiler` and `BCVMState` themselves are no longer allocated from `bc_heap` on device; they are explicit device-static state.
  - The VM runner no longer resets `bc_heap` on normal entry/exit because the caller's source buffer may live in that arena.
  - Arena-owned source text is released immediately after successful compile, before compiler compaction and VM execution.
  - Source file loading now allocates based on actual file size instead of reserving the maximum edit buffer for every `.bas` file.
  - `bc_vm_alloc()` now runs after successful compile and compiler compaction, removing VM runtime tables from compile-time peak memory.
  - RP2350 `bc_heap` is reduced from 256 KiB to 232 KiB.
  - `MEMORY` on PicoCalc RP2350 now reports VM arena capacity, current use, and high-water usage for device-driven sizing.
  - Large compiler/runtime tables still allocate through `BC_ALLOC()`; compile/runtime arena separation remains future work.
  - Rebuild and measure `.bss`/headroom after each slice.

### 2A. Build a VM-owned source frontend

Status: started.

- Add a raw-source compiler entrypoint that emits bytecode directly.
- Do not depend on `tokenise()`, `commandtbl`, `tokentbl`, `cmd_*`, `fun_*`, or handler function pointers for device compilation.
- Keep the old tokenized compiler only as a temporary compatibility path while the new frontend reaches feature parity.
- Host differential testing should compare:
  - `source -> legacy interpreter`
  - `source -> VM source frontend -> bytecode -> VM`
- First implemented slice:
  - `bc_source.c` / `bc_source.h`
  - host `--vm-source` mode
  - host `--source-compare` mode
  - `PRINT` with literal integer, float, string, parenthesized string concatenation, `?`, `;`, and colon-separated statements
- Next frontend slices should migrate language constructs before any more syscall expansion:
  - scalar variables and assignment
  - arithmetic/comparison expression precedence
  - `IF`/`THEN`/`ELSE`
  - `FOR`/`NEXT`
  - `GOTO`/labels/line references
  - `DIM` and arrays
  - `SUB`/`FUNCTION`

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
  - load source
  - compile source through the VM-owned frontend
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
- New VM syscall support should not call through legacy interpreter command/function handlers.
- If old implementation code is needed, copy/adapt the minimum required logic into a `vm_sys_*` native runtime module and keep legacy behavior available for host oracle comparison.
- This is a hard rule: VM syscalls must not wrap existing legacy syscall functions. A conversion is not complete if the VM opcode reaches back into `cmd_*`, `fun_*`, `Ext*`, old drawing handlers, or old file command handlers.
- `OP_BUILTIN_*` bridge opcodes and VM handlers have been removed.
- The old `bc_bridge.c` implementation has been removed and the remaining VM runtime entrypoints live in `bc_runtime.c`.
- Unsupported commands/functions now fail at compile time with `Unsupported VM command: ...` or `Unsupported VM function: ...`.
- Negative tests live under `host/tests/unsupported/` and are run by `host/run_unsupported_tests.sh`.
- Current graphics VM syscalls dispatch through the VM-owned `vm_sys_graphics.c` module: `BOX`, `RBOX`, `ARC`, `TRIANGLE`, `POLYGON`, `CIRCLE`, `CLS`, `LINE`, `PIXEL`, `TEXT`, `FONT`, and `COLOUR`/`COLOR`.
- The RP2350 `CIRCLE` path no longer calls the legacy `DrawCircle()` implementation that allocates scratch storage through `GetTempMemory()`.
- `DATE$` and `TIME$` now dispatch through `vm_sys_time.c`; `KEYDOWN()` dispatches through `vm_sys_input.c`.
- `MM.INFO(HRES)` and `MM.INFO(VRES)` compile to VM-native display-size queries.
- `PLAY TONE`/`PLAY STOP` dispatch through `vm_sys_audio.c`.
- `SETPIN`/`PIN()` digital forms dispatch through `vm_sys_pin.c`, which owns copied/adapted GPIO mapping/config/read/write logic instead of calling legacy `ExtCfg`/`ExtSet`/`ExtInp`.

### 6. Use host differential testing as the main validation loop

Status: active.

- `legacy host interpreter` is the semantic oracle.
- `host VM` is the implementation target.
- Compare outputs, errors, and host-rendered framebuffers where meaningful.
- Oracle tests must stay within the syntax and behavior accepted by the legacy host interpreter.
- If the legacy interpreter rejects a construct, that test is not a valid oracle-comparison test until it is rewritten into a legacy-valid form.
- Use device tests mainly for hardware-facing behavior, timing, and memory/resource validation.
- Current baseline:
  - `./host/run_tests.sh`: 120 passing oracle comparison tests.
  - `./host/run_pixel_tests.sh`: passing framebuffer assertions.
  - `./host/run_host_shim_tests.sh`: 3 passing host shim tests.
  - `./host/run_frontend_tests.sh`: 27 passing source frontend tests.
  - `bash host/run_unsupported_tests.sh`: 1 passing unsupported-syscall negative test.
  - `./host/run_missing_syscall_tests.sh`: intentionally red, currently 2 failing syscall TODOs: `FILES` and file I/O.

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
./host/run_host_shim_tests.sh
./host/run_frontend_tests.sh
bash host/run_unsupported_tests.sh
./host/run_missing_syscall_tests.sh   # intentionally red until syscall TODOs are implemented
make -C build2350 -j8
arm-none-eabi-size build2350/PicoMite.elf
```

Current firmware size snapshot after `PLAY` and digital `PIN`/`SETPIN` native syscall slices:

```text
text=929992  data=0  bss=295808  dec=1225800
bc_heap=232 KiB
```

## Success Criteria

- Device firmware contains one execution engine for BASIC programs: the VM.
- Device prompt no longer executes arbitrary BASIC.
- Host retains an untouched interpreter for semantic regression testing.
- Missing VM functionality fails loudly instead of silently bridging.

## Remaining Work

- Continue removing interpreter-only runtime from the device image where it is no longer needed by shell/tokenising/editor workflows.
- Expand the VM-owned source frontend until the device `RUN` path no longer needs legacy tokenising.
- After the frontend cut is in place, continue native VM syscall coverage only where the legacy interpreter supports the same BASIC semantics, implemented outside the legacy interpreter modules.
- Graphics is the highest-priority command-family backlog.
- **!!! GRAPHICS CONVERSIONS MUST COPY/ADAPT LEGACY DRAWING CODE INTO VM-OWNED HELPERS AND SYSCALLS !!!**
- **!!! GRAPHICS CONVERSIONS MUST NOT LINK, WRAP, OR DISPATCH THROUGH OLD DRAWING HANDLERS OR COMMAND ENTRYPOINTS !!!**
- Use `host/tests/missing_syscalls/` for fail-first syscall work; move or delete entries when native VM support lands.
- Deferred syscall extraction target is `vm_sys_file.c` for `OPEN`/`CLOSE`/file print-input/`FILES`.
- Expand `vm_sys_pin.c` beyond digital `OFF`/`DIN`/`DOUT` only by copying/adapting each supported legacy mode into the VM-owned module with fail-first oracle/device coverage.
- Replace the temporary PicoCalc/RP2350 allocator compatibility layer by migrating surviving prompt/device services onto explicit VM/system allocation APIs or removing the legacy modules that still require it.
- Keep unsupported VM functionality as loud compile/runtime faults with regression coverage.
- Add device-specific tests for LCD clearing, FASTGFX swaps, frame pacing, SD interactions, and long-running memory pressure.

## Legacy Command Coverage Inventory

Status terms in this section are strict:

- `implemented`: frontended and executable on the VM program path.
- `partial`: frontended, but only a subset of the legacy command semantics is implemented.
- `unimplemented`: not available on the VM program path yet. This includes commands that still exist only in legacy command handlers and commands with no VM frontend support.

This inventory is command-table driven from `AllCommands.h`. It is about the VM BASIC program path, not the prompt shell. Function-family gaps such as the wider `MM.INFO()` surface are tracked separately from command-table entries.

### Graphics Priority

Graphics is the current highest-priority conversion area.

- First priority is the drawing/syscall surface used by real programs and validation assets.
- Each conversion must be VM-owned and must copy/adapt useful legacy code into `vm_sys_*` modules.
- A conversion is not complete if the VM opcode reaches back into `cmd_*`, `fun_*`, old drawing helpers, or other legacy execution handlers.
- `implemented`, `partial`, and `unimplemented` below are the only status terms for this inventory.

### Implemented

- Core language/control flow:
  `DATA`, `DIM`, `DO`, `ELSE IF`, `ELSEIF`, `CASE ELSE`, `ELSE`, `SELECT CASE`, `END SELECT`, `CASE`, `END IF`, `ENDIF`, `END FUNCTION`, `END SUB`, `END`, `ERROR`, `EXIT DO`, `EXIT FOR`, `EXIT SUB`, `EXIT FUNCTION`, `EXIT`, `FOR`, `FUNCTION`, `GOSUB`, `GOTO`, `INC`, `IF`, `LINE INPUT`, `LET`, `LOCAL`, `LOOP`, `NEXT`, `PRINT`, `READ`, `RESTORE`, `RETURN`, `STATIC`, `SUB`, `CONST`, `RANDOMIZE`
- VM graphics/display path:
  `PIXEL`, `CIRCLE`, `LINE`, `BOX`, `RBOX`, `ARC`, `TRIANGLE`, `POLYGON`, `CLS`, `FASTGFX`, `TEXT`, `FONT`, `COLOUR`, `COLOR`
- VM file/time/input path:
  `PAUSE`, `DATE$`, `TIME$`

### Partial

- `OPTION`
  Only the source forms already needed by the host/device VM path are handled, not the broad legacy `OPTION` command surface.
- `SAVE`
  Only `SAVE IMAGE` is frontended.
- `OPEN`
  Only sequential file forms are implemented: `INPUT`, `OUTPUT`, `APPEND`.
  Random-access files, `COM`, `GPS`, and other legacy forms are unimplemented.
- `CLOSE`
  Only file-handle close is implemented.
  Other legacy resource-family closes remain unimplemented because their owning command families are unimplemented.
- `PLAY`
  Only `PLAY TONE` and `PLAY STOP` are implemented.
  The wider `PLAY` family (`WAV`, `FLAC`, `MP3`, `MODFILE`, `ARRAY`, `SOUND`, `PAUSE`, `RESUME`, `VOLUME`, etc.) is unimplemented.
- `PIN(`
  Basic digital read/write is implemented on the VM path.
  The full legacy `PIN()` function family is not.
- `SETPIN`
  Only the basic digital slice is implemented: `OFF`, `DIN`, `DOUT`.
  Analog, counters, interrupts, peripheral allocation, PWM, IR, SPI, I2C, serial, and other legacy modes are unimplemented.
- `TIMER`
  The function form is implemented.
  The broader legacy command/form-family behavior is not fully matched.
- `FILES`
  Frontended, but the VM runtime implementation is still empty on device and host.
- Graphics command family
  The VM covers the current primitive subset plus the first copied/adapted shape/state commands: `PIXEL`, `CIRCLE`, `LINE`, `BOX`, `RBOX`, `ARC`, `TRIANGLE`, `POLYGON`, `CLS`, `FASTGFX`, `TEXT`, `FONT`, `COLOUR`, `COLOR`.
  The broader legacy graphics/display surface remains unimplemented and is tracked explicitly below.

### Unimplemented

- Core/editor/program control:
  `CALL`, `CLEAR`, `CONTINUE`, `ERASE`, `INPUT`, `LIST`, `LOAD`, `ON`, `RUN`, `TRACE`, `WHILE`, `EXECUTE`, `NEW`, `EDIT FILE`, `EDIT`, `AUTOSAVE`
- File/OS/storage commands:
  `KILL`, `RMDIR`, `CHDIR`, `MKDIR`, `COPY`, `RENAME`, `SEEK`, `FLASH`, `VAR`, `FLUSH`, `DRIVE`, `XMODEM`, `CAT`
- Graphics/UI/display families:
  `FRAMEBUFFER`, `SPRITE`, `BLIT`, `BLIT MEMORY`, `GUI`, `SYNC`, `DEVICE`, `LCD`, `REFRESH`, `BACKLIGHT`, `DRAW3D`, `TILE`, `MODE`, `MAP(`, `MAP`, `COLOUR MAP`, `CAMERA`, `CTRLVAL(`
- Peripheral/comms/hardware families:
  `ADC`, `PULSE`, `PORT(`, `IR`, `PWM`, `CSUB`, `END CSUB`, `I2C`, `I2C2`, `RTC`, `MATH`, `MEMORY`, `IRETURN`, `POKE`, `SETTICK`, `WATCHDOG`, `CPU`, `SORT`, `DEFINEFONT`, `END DEFINEFONT`, `LONGSTRING`, `INTERRUPT`, `LIBRARY`, `ONEWIRE`, `TEMPR START`, `SPI`, `SPI2`, `WS2812`, `KEYPAD`, `HUMID`, `WII CLASSIC`, `WII NUNCHUCK`, `WII`, `SERVO`, `MOUSE`, `CHAIN`, `WEB`, `GAMEPAD`, `UPDATE FIRMWARE`, `CONFIGURE`, `CMM2 LOAD`, `CMM2 RUN`, `RAM`
- PIO/assembler families:
  `PIO`, `_wrap target`, `_wrap`, `_line`, `_program`, `_end program`, `_side set`, `_label`, `Jmp`, `Wait`, `In`, `Out`, `Push`, `Pull`, `Mov`, `Nop`, `IRQ SET`, `IRQ WAIT`, `IRQ CLEAR`, `IRQ NOWAIT`, `IRQ`, `Set`
- Command/function hybrids and utility families:
  `BYTE(`, `FLAG(`, `FRAME`, `BIT(`, `FLAGS`, `HELP`, `ARRAY SLICE`, `ARRAY INSERT`, `ARRAY ADD`, `ARRAY SET`

Notes:

- `FILES` is the only command-table entry currently frontended into the VM and still implemented as a runtime no-op.
- `WS2812` is unimplemented in the VM sense even though a legacy `cmd_WS2812()` exists in `External.c`; it has no VM frontend/runtime path yet.
- The command table does not capture the full function backlog. For example, `MM.INFO()` has a large legacy surface, but only the current VM-required display-size subset is implemented.
- Graphics is the first command-family expansion target from the `unimplemented` list, ahead of broader file/peripheral work.
