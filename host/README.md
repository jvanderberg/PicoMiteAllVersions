# MMBasic Host Build

Native macOS/Linux build of the MMBasic interpreter and bytecode VM
for off-device testing. Runs .bas programs on your desktop without hardware.

## Quick Start

```bash
cd host/
./build.sh                              # build
./run_tests.sh                          # run all 49 tests (compare mode)
./run_bench.sh                          # run benchmarks
./mmbasic_test program.bas              # run a single program (compare both engines)
```

## Scripts

| Script | Purpose |
|--------|---------|
| `build.sh` | Incremental build. `./build.sh clean` for full rebuild. |
| `run_tests.sh` | Run test suite. Default: compare both engines. `--interp` or `--vm` for single engine. |
| `run_bench.sh` | Run benchmarks with timing. `./run_bench.sh mandelbrot` for a single benchmark. |

## What This Is

The PicoMite firmware normally runs on RP2040/RP2350 microcontrollers. This host
build compiles the core MMBasic language runtime (tokenizer, expression evaluator,
commands, functions, operators, memory manager) as a native desktop binary, with
stub implementations for all hardware-dependent code (GPIO, SPI, I2C, display,
audio, filesystem, etc.).

The primary purpose is **regression testing the bytecode VM** against the existing
interpreter. Both engines process the same tokenized program; the test harness
captures printed output from each and compares the results.

## Architecture

```
host/
├── Makefile            # Build system
├── build.sh            # Build script (incremental or clean)
├── run_tests.sh        # Test runner (compare, --interp, --vm)
├── run_bench.sh        # Benchmark runner with timing
├── host_platform.h     # Force-included header: defines PICOMITE, stubs SDK macros
├── host_main.c         # Test driver: load .bas, run engines, compare output
├── host_stubs.c        # ~700 lines of stub globals + functions for hardware code
├── lfs.h               # LittleFS stub header
├── hardware/           # Stub Pico SDK headers (dma.h, irq.h, spi.h, etc.)
├── pico/               # Stub Pico SDK headers (stdlib.h, multicore.h, etc.)
├── tests/              # Test .bas programs (49 tests)
│   ├── t01_print.bas ... t49_misc_stmts.bas
└── bench_*.bas         # Benchmark programs
    ├── bench_fibonacci.bas
    ├── bench_mandelbrot.bas
    ├── bench_matrix.bas
    └── bench_sieve.bas
```

### What Gets Compiled

| Source | Origin | Purpose |
|--------|--------|---------|
| `MMBasic.c` | parent dir | Tokenizer, expression evaluator, core runtime |
| `Commands.c` | parent dir | BASIC command implementations (PRINT, FOR, IF, etc.) |
| `Functions.c` | parent dir | BASIC function implementations (LEFT$, LEN, etc.) |
| `Operators.c` | parent dir | Operator evaluation |
| `MATHS.c` | parent dir | Math functions, RNG, CRC |
| `Memory.c` | parent dir | Heap/stack memory manager |
| `bc_*.c` | parent dir | Bytecode compiler + VM (7 files) |
| `host_stubs.c` | host/ | Stubs for ~300 hardware-dependent symbols |
| `host_main.c` | host/ | Test driver main() |

### What Does NOT Get Compiled

Everything hardware-specific: `PicoMite.c`, `Draw.c`, `SPI-LCD.c`, `FileIO.c`,
`External.c`, `Audio.c`, `Editor.c`, `GPS.c`, `I2C.c`, `SPI.c`, `Serial.c`,
`Custom.c`, `XModem.c`, `ff.c`, `Onewire.c`, `BDSP.c`, `picojpeg.c`, etc.

These are replaced by stub definitions in `host_stubs.c`.

## How It Works

### Program Loading (`host_main.c`)

1. Reads a `.bas` file as plain text
2. For each non-empty line, prepends the line number and calls `tokenise(0)`
3. Copies the tokenized output from `tknbuf` to `ProgMemory`
4. Terminates with two zero bytes (standard MMBasic program format)

This mirrors `SaveProgramToFlash()` in `PicoMite.c`.

### Tokenized Data Copy

The tokenizer terminates `tknbuf` with three zero bytes. Individual program
elements in `ProgMemory` are separated by single zero bytes. However, `T_LINENBR`
tokens contain embedded zero bytes (the high byte of line numbers < 256), so the
copy loop uses the two-consecutive-zeros terminator check:

```c
while (!(tp[0] == 0 && tp[1] == 0)) {
    *pm++ = *tp++;
}
*pm++ = 0;  // element terminator
```

### Flash Memory Simulation

On the real device, `flash_progmemory` points to SPI flash. On the host, we
allocate a 256KB buffer:
- First 128KB: zeroed (program area)
- Second 128KB: filled with `0xFF` (mimics erased flash)

The `0xFF` fill is critical -- `PrepareProgramExt` walks the CFunction area after
the program and expects `0xFFFFFFFF` as the terminator for erased flash.

### Output Capture

The test harness redirects MMBasic output via a function pointer hook
(`host_output_hook`). When capturing, `MMPrintString`/`MMputchar`/`PRet` etc.
write to a buffer instead of stdout. This allows clean comparison of output from
different execution engines.

### Execution Modes

```bash
./mmbasic_test program.bas              # Compare both engines
./mmbasic_test program.bas --interp     # Interpreter only
./mmbasic_test program.bas --vm         # Bytecode VM only
```

## Key Portability Fixes

### `-funsigned-char`

ARM (the real target) defaults to unsigned `char`. macOS/x86 defaults to signed
`char`. Without `-funsigned-char`, the tokenizer's `STR_REPLACE` function fails
to convert `0xFF` bytes back to spaces inside string literals, because:

```c
char *ip = ...;       // signed char on macOS
if (*ip == 0xFF) ...  // compares -1 with 255 -> always false!
```

### 64-bit Pointer Safety

`MMBasic.c:461` had `(unsigned int)p` for pointer alignment, which truncates on
64-bit hosts. Fixed to `(uintptr_t)p`.

### Stack Overflow Check

`TestStackOverflow()` in `Memory.c` calls `__get_MSP()` (ARM stack pointer) and
checks it against `HEAPTOP`. The stub returns `0xFFFFFFFF` so the check always
passes.

## Adding New Tests

Create a `.bas` file in `tests/` following the naming convention `tNN_description.bas`.
The test runner picks up all `tests/t*.bas` files automatically.

Tests should produce deterministic printed output. The comparison mode runs both
engines and diffs the output character-by-character.

## Adding New Benchmarks

Create a `.bas` file in the `host/` directory named `bench_NAME.bas`.
The benchmark runner picks up all `bench_*.bas` files automatically.

Benchmarks should do meaningful computation. Include `TIMER` for internal timing,
but the benchmark script uses wall-clock `time` for accurate comparison.

## Current Status

- **49 tests**: All passing in compare mode (interpreter output == VM output)
- **Benchmarks** (host macOS, Apple Silicon):

| Benchmark | Interpreter | VM | Speedup |
|-----------|-------------|-----|---------|
| Fibonacci(30) | 5.9s | 0.23s | ~25x |
| Mandelbrot 161x161 | 1.5s | 0.11s | ~13x |
| Matrix 41x41 multiply | 0.10s | 0.01s | ~8x |
| Sieve x10 | 0.10s | 0.02s | ~4x |
