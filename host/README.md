# MMBasic Host Build

Native macOS/Linux test harness for the legacy MMBasic interpreter and the bytecode VM.

For the current architecture snapshot, see:

- [docs/vm-architecture.md](../docs/vm-architecture.md)
- [docs/vm-command-coverage.md](../docs/vm-command-coverage.md)

The host binary is `mmbasic_test`. It has two execution engines:

- `--interp`: legacy interpreter oracle.
- `--vm`: bytecode VM implementation.
- `--vm-source`: VM-owned raw-source frontend to bytecode to VM.
- `--source-compare`: legacy interpreter oracle compared with raw-source VM frontend.
- default: run both and compare output.

The device uses VM-owned BASIC program execution. The host interpreter remains as the semantic reference for language behavior and host-safe syscalls.

## Quick Start

```bash
cd host
./build.sh
./run_tests.sh
./run_pixel_tests.sh
./run_host_shim_tests.sh
./run_frontend_tests.sh
./run_optimizer_tests.sh
./run_unsupported_tests.sh
./run_missing_syscall_tests.sh
```

Equivalent from the repo root:

```bash
make -C host
./host/run_tests.sh
./host/run_pixel_tests.sh
./host/run_host_shim_tests.sh
./host/run_frontend_tests.sh
./host/run_optimizer_tests.sh
bash host/run_unsupported_tests.sh
./host/run_missing_syscall_tests.sh
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
| `./run_tests.sh` | Runs all `tests/t*.bas` oracle tests in compare mode. Current count: 168. |
| `./run_tests.sh --interp` | Runs oracle tests through the legacy interpreter only. |
| `./run_tests.sh --vm` | Runs oracle tests through the VM. |
| `./run_tests.sh tests/t01_print.bas --vm` | Runs one test through one engine. |
| `./run_pixel_tests.sh` | Runs framebuffer assertions through both interpreter and VM. |
| `./run_host_shim_tests.sh` | Runs deterministic host shim tests, including fixed date/time and delayed keyboard injection. Current count: 4. |
| `./run_frontend_tests.sh` | Runs raw-source VM frontend oracle comparisons. Current count: 48. |
| `./run_optimizer_tests.sh` | Runs peephole/superinstruction assertion and equivalence tests. Current count: 22. |
| `./run_unsupported_tests.sh` | Negative tests for unsupported VM syscalls. Current count: 0. |
| `./run_missing_syscall_tests.sh` | Inventory runner for missing VM syscall implementations. Current count: 0. |
| `./run_bench.sh` | Runs host benchmarks. |

`BINARY=...` can override the binary used by the scripts, but the supported default is `./mmbasic_test`.

Keyboard-driven host tests can inject characters with `--keys TEXT` or `--keys-after-ms MS TEXT`. The key string supports escapes such as `\n`, `\r`, `\t`, `\\`, and `\xNN`.

## Test Policy

Oracle comparison tests must be accepted by the legacy interpreter. Do not add VM-specific syntax or VM extensions to `tests/t*.bas`; if the interpreter rejects it, it is not a valid semantic oracle test.

Unsupported syscall tests belong in `tests/unsupported/*.bas`. Each file must fail under `--vm` and must start with:

```basic
' EXPECT_ERROR: expected substring
```

Missing syscall implementation tests belong in `tests/missing_syscalls/*.bas`. These are normal programs that should run under the interpreter oracle but expose native VM syscall gaps. The suite is currently empty and exits cleanly when there are no pending missing-syscall cases.

Host shim tests belong in `tests/host_shims/*.bas`. These are for host harness behavior rather than MMBasic semantic coverage.

Source frontend tests belong in `tests/frontend/*.bas`. These compare `source -> legacy interpreter` against `source -> VM source frontend -> bytecode -> VM`; they should grow before adding more native syscall coverage.

Optimizer tests belong in `tests/frontend/` and are run by `run_optimizer_tests.sh`. Each optimization has two test files:
- **Peephole tests** (`t0XX_*_peephole.bas`): verify the fused opcode appears in `--vm-disasm` output and the unfused form is absent.
- **Equivalence tests** (`t0XX_*_opt_equiv.bas`): compare `-O0` vs `-O1` output to verify correctness across boundary values, sign combinations, and type edge cases.

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
| `bc_source.c`, `bc_compiler*.c`, `bc_vm.c`, `bc_runtime.c`, `bc_debug.c`, `bc_test.c` | VM-owned source frontend, compiler/core metadata, VM, runtime entrypoints, diagnostics, and internal tests. |
| `host_stubs_legacy.c` | Host shim for hardware, filesystem, display, timers, input, and interpreter support. Some oracle-independence cleanup is still pending here. |
| `host_main.c` | Loads `.bas`, tokenises into `ProgMemory`, runs engines, compares/captures output. |

The old bridge fallback is removed. The VM compiler must emit native bytecode for supported statements/functions; unsupported commands/functions must fail loudly.

## Program Loading

Default compare mode still reads a `.bas` file as text, prepends generated line numbers when needed, calls `tokenise(0)`, copies tokenised data into `ProgMemory`, and terminates with the standard double-zero program terminator.

The interpreter side still tokenises through the legacy path. The VM side uses `bc_compile_source()` directly on raw `.bas` text for `--vm` and `--source-compare`.

## Output And Framebuffer Capture

The host shim redirects console output through `host_output_hook` so interpreter and VM output can be compared exactly.

Graphics tests use the host framebuffer and `--assert-pixel x,y,RRGGBB` arguments to verify deterministic drawing behavior. The same `.bas` test is run under both engines.

## Current Verification Snapshot

Current snapshot:

- `make -C host`: passes.
- `make -C build2350 -j8`: passes.
- `./host/run_tests.sh`: `168 passed, 0 failed`.
- `./host/run_pixel_tests.sh`: passes.
- `./host/run_host_shim_tests.sh`: `4 passed, 0 failed`.
- `./host/run_frontend_tests.sh`: `48 passed, 0 failed`.
- `./host/run_optimizer_tests.sh`: `22 passed, 0 failed`.
- `bash host/run_unsupported_tests.sh`: `0 passed, 0 failed`.
- `./host/run_missing_syscall_tests.sh`: `0 passed, 0 failed`.

## Known Build Note

The host Makefile is intentionally small and does not track all header dependencies. If command table or shared header changes produce a stale-object link error, run:

```bash
make -B -C host
```
