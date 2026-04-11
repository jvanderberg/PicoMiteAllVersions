# MMBasic Host Build

Native macOS/Linux test harness for the legacy MMBasic interpreter and the bytecode VM.

The host binary is `mmbasic_test`. It has two execution engines:

- `--interp`: legacy interpreter oracle.
- `--vm`: bytecode VM implementation.
- `--vm-source`: VM-owned raw-source frontend to bytecode to VM.
- `--source-compare`: legacy interpreter oracle compared with raw-source VM frontend.
- default: run both and compare output.

The device direction is VM-only for BASIC program execution. The host interpreter remains as the semantic reference for language behavior and host-safe syscalls.

## Quick Start

```bash
cd host
./build.sh
./run_tests.sh
./run_pixel_tests.sh
./run_host_shim_tests.sh
./run_frontend_tests.sh
./run_unsupported_tests.sh
./run_missing_syscall_tests.sh        # intentionally red syscall TODO inventory
```

Equivalent from the repo root:

```bash
make -C host
./host/run_tests.sh
./host/run_pixel_tests.sh
./host/run_host_shim_tests.sh
./host/run_frontend_tests.sh
bash host/run_unsupported_tests.sh
./host/run_missing_syscall_tests.sh   # intentionally exits non-zero until TODOs are implemented
```

## Running Programs

```bash
./mmbasic_test program.bas
./mmbasic_test program.bas --interp
./mmbasic_test program.bas --vm
./mmbasic_test program.bas --vm-source
./mmbasic_test program.bas --source-compare
```

The default compare mode runs the legacy interpreter first, then the VM, and fails if stdout or error behavior differs.
`--source-compare` is the migration harness for removing device dependence on the legacy tokenizer: the interpreter still uses the legacy tokenized path as oracle, while the VM compiles directly from raw `.bas` source.

## Test Suites

| Command | Purpose |
|---------|---------|
| `./run_tests.sh` | Runs all `tests/t*.bas` oracle tests in compare mode. Current count: 118. |
| `./run_tests.sh --interp` | Runs oracle tests through the legacy interpreter only. |
| `./run_tests.sh --vm` | Runs oracle tests through the VM only. |
| `./run_tests.sh tests/t01_print.bas --vm` | Runs one test through one engine. |
| `./run_pixel_tests.sh` | Runs framebuffer assertions through both interpreter and VM. |
| `./run_host_shim_tests.sh` | Runs deterministic host shim tests, including fixed date/time and delayed keyboard injection. Current count: 3. |
| `./run_frontend_tests.sh` | Runs raw-source VM frontend oracle comparisons. Current count: 2. |
| `./run_unsupported_tests.sh` | VM-only negative tests for unsupported syscalls. Current count: 2. |
| `./run_missing_syscall_tests.sh` | Intentionally-red compare tests for missing VM syscall implementations. Current count: 4. |
| `./run_bench.sh` | Runs host benchmarks. |

`BINARY=...` can override the binary used by the scripts, but the supported default is `./mmbasic_test`.

Keyboard-driven host tests can inject characters with `--keys TEXT` or `--keys-after-ms MS TEXT`. The key string supports escapes such as `\n`, `\r`, `\t`, `\\`, and `\xNN`.

## Test Policy

Oracle comparison tests must be accepted by the legacy interpreter. Do not add VM-only syntax or VM extensions to `tests/t*.bas`; if the interpreter rejects it, it is not a valid semantic oracle test.

Unsupported syscall tests belong in `tests/unsupported/*.bas`. Each file must fail under `--vm` and must start with:

```basic
' EXPECT_ERROR: expected substring
```

Missing syscall implementation tests belong in `tests/missing_syscalls/*.bas`. These are normal programs that should run under the interpreter oracle, but currently fail in compare mode because the VM has no native syscall implementation yet. This suite is expected to exit non-zero until those syscalls are implemented.

Host shim tests belong in `tests/host_shims/*.bas`. These are for host harness behavior rather than MMBasic semantic coverage.

Source frontend tests belong in `tests/frontend/*.bas`. These compare `source -> legacy interpreter` against `source -> VM source frontend -> bytecode -> VM`; they should grow before adding more native syscall coverage.

Graphics tests should prefer deterministic framebuffer assertions in `run_pixel_tests.sh`. Host graphics are still an approximation of hardware, so final validation for FASTGFX, LCD behavior, timing, SD, and resource limits must happen on device.

## Architecture

```text
host/
├── Makefile
├── build.sh
├── run_tests.sh
├── run_pixel_tests.sh
├── run_frontend_tests.sh
├── run_unsupported_tests.sh
├── run_bench.sh
├── host_platform.h
├── host_main.c
├── host_stubs_legacy.c
├── tests/
│   ├── t*.bas
│   ├── frontend/
│   ├── host_shims/
│   ├── unsupported/
│   └── missing_syscalls/
└── mmbasic_test
```

Important compiled sources:

| Source | Purpose |
|--------|---------|
| `MMBasic.c`, `Commands.c`, `Functions.c`, `Operators.c`, `MATHS.c`, `Memory.c` | Legacy language runtime used by the host oracle and current prompt/tokenising paths. |
| `gfx_*_shared.c` | Shared host/device graphics primitives used by native VM graphics ops. |
| `bc_source.c`, `bc_compiler*.c`, `bc_vm.c`, `bc_runtime.c`, `bc_debug.c`, `bc_test.c` | VM-owned source frontend, legacy-tokenized compatibility compiler, VM, runtime entrypoints, diagnostics, and internal tests. |
| `host_stubs_legacy.c` | Host shim for hardware, filesystem, display, timers, input, and VM support. |
| `host_main.c` | Loads `.bas`, tokenises into `ProgMemory`, runs engines, compares/captures output. |

The old bridge fallback is removed. The VM compiler must emit native bytecode for supported statements/functions; unsupported commands/functions must fail loudly.

## Program Loading

Default compare mode still reads a `.bas` file as text, prepends generated line numbers when needed, calls `tokenise(0)`, copies tokenised data into `ProgMemory`, and terminates with the standard double-zero program terminator.

This keeps host tests close to device `RUN`, where source is tokenised before VM compilation.

The new frontend migration path uses `--vm-source` / `--source-compare`, which call `bc_compile_source()` directly on raw `.bas` text and do not use `tokenise()` for the VM side.

## Output And Framebuffer Capture

The host shim redirects console output through `host_output_hook` so interpreter and VM output can be compared exactly.

Graphics tests use the host framebuffer and `--assert-pixel x,y,RRGGBB` arguments to verify deterministic drawing behavior. The same `.bas` test is run under both engines.

## Current Verification Snapshot

As of the VM bridge-removal checkpoint:

- `make -C host`: passes.
- `make -C build2350 -j8`: passes.
- `./host/run_host_shim_tests.sh`: `3 passed, 0 failed`.
- `./host/run_frontend_tests.sh`: `2 passed, 0 failed`.
- `bash host/run_unsupported_tests.sh`: `2 passed, 0 failed`.
- `./host/run_missing_syscall_tests.sh`: intentionally red, currently 4 failing syscall TODOs.
- `./host/run_tests.sh`: `118 passed, 0 failed`.
- `./host/run_pixel_tests.sh`: passes.
- `arm-none-eabi-size build2350/PicoMite.elf`: `text=942400`, `data=0`, `bss=314180`, `dec=1256580`.

## Known Build Note

The host Makefile is intentionally small and does not track all header dependencies. If command table or shared header changes produce a stale-object link error, run:

```bash
make -B -C host
```
