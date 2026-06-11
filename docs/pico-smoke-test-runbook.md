# Pico and PicoCalc Smoke Test Runbook

This document is the hardware smoke-test checklist for Pico-family builds. It
is intended for PR validation and regression triage on real boards, especially
PicoCalc RP2040/RP2350 and PicoCalc WiFi variants.

Run commands from the repo root unless noted otherwise.

## Scope

The full Pico smoke pass should answer four questions:

1. Does every PicoCalc firmware variant still build?
2. Does the flashed board boot to a responsive MMBasic prompt?
3. Do board-specific peripherals work end-to-end on real hardware?
4. Do risky low-level APIs survive negative, callback, DMA, and teardown paths?

Host tests and compile-only gates are not a substitute for this runbook. The
PicoCalc LCD, SD path, keyboard MCU, PSRAM, PIO DMA, USB serial, and WiFi stack
all have timing and teardown behavior that only shows up on hardware.

## Requirements

- Python with `pyserial`; this repo has generally used `python3.11`.
- `picotool` for BOOTSEL flashing when available.
- A known serial port, for example `/dev/cu.usbmodem1101` on macOS.
- For WiFi boards, saved WiFi options or a `WEB CONNECT` command.
- For SD tests, an inserted card mounted by the firmware as `B:`.
- For PSRAM tests, the correct `OPTION PSRAM PIN ...` configuration.

Useful shell setup:

```sh
export PORT=/dev/cu.usbmodem1101
export PY=python3.11
```

Quick prompt check:

```sh
$PY porttools/basic_serial.py \
  --port "$PORT" \
  --boot-wait 1 \
  --cmd 'PRINT MM.DEVICE$' \
  --cmd 'PRINT MM.INFO$(ID)'
```

## Build Matrix

For PicoCalc PR validation, build all four PicoCalc variants:

```sh
cmake --build build_picocalc_rp2040 -j
cmake --build build_picocalc_wifi_rp2040 -j
cmake --build build_picocalc_rp2350 -j
cmake --build build_picocalc_wifi_rp2350 -j
```

When build directories are missing or stale, use the project build helper or
configure the matching CMake build directories first.

Expected UF2 outputs:

```text
build_picocalc_rp2040/PicoMite.uf2
build_picocalc_wifi_rp2040/PicoMite.uf2
build_picocalc_rp2350/PicoMite.uf2
build_picocalc_wifi_rp2350/PicoMite.uf2
```

## Flashing

If BASIC is responsive, prefer entering BOOTSEL from the prompt:

```sh
$PY porttools/basic_serial.py \
  --port "$PORT" \
  --timeout 5 \
  --long-timeout 10 \
  --cmd 'UPDATE FIRMWARE' || true
```

The serial command may exit nonzero because USB intentionally disconnects. Wait
for the BOOTSEL volume, then copy the UF2:

```sh
# Volume name is usually RPI-RP2 on RP2040 and RP2350 on RP2350.
cp -X build_picocalc_wifi_rp2350/PicoMite.uf2 /Volumes/RP2350/PicoMite.uf2
sync
```

On macOS, use `cp -X` to avoid extended-attribute errors on the UF2 volume.
The volume disappearing after a copy usually means the bootloader accepted the
firmware and rebooted.

If the prompt is wedged, use manual BOOTSEL/reset or SWD. `CPU RESTART` only
reboots the app and does not enter BOOTSEL.

After flashing:

```sh
$PY porttools/basic_serial.py \
  --port "$PORT" \
  --boot-wait 1 \
  --cmd 'PRINT MM.DEVICE$' \
  --cmd 'PRINT MM.VER' \
  --cmd 'PRINT MM.INFO$(ID)'
```

## Smoke Tiers

### Tier 0: Prompt and Device Identity

Run this after every flash:

```sh
$PY porttools/basic_serial.py \
  --port "$PORT" \
  --boot-wait 1 \
  --cmd 'PRINT "DEVICE="+MM.DEVICE$' \
  --cmd 'PRINT "ID="+MM.INFO$(ID)' \
  --cmd 'PRINT "HEAP="+STR$(MM.INFO(HEAP))'
```

Check that the reported device matches the board in the socket. Do not continue
with a board-specific smoke if the wrong variant is flashed.

### Tier 1: Core PicoCalc Smoke

This is the broad filesystem, BASIC, VM, display, and device smoke:

```sh
$PY porttools/pico_fs_vm_smoke.py all \
  --port "$PORT" \
  --long-timeout 60
```

What it covers:

- `MM.INFO` identity, CPU speed, free space, battery and charging state.
- A: filesystem create/read/write/append/copy/rename/chdir/dir/kill/rmdir.
- Filesystem error paths.
- Multi-sector file write/read checksums.
- `LOAD`, `SAVE`, `RUN`, autorun `LOAD ..., R`.
- `FLASH ERASE/SAVE/LIST/LOAD/RUN` on the selected scratch slot.
- `AUTOSAVE`.
- Bytecode VM and `FRUN` paths.
- Integer and floating-point arithmetic, arrays, subs/functions, `DATA`.
- Sieve benchmark.
- `TIMER`, `PAUSE`, `SETTICK`.
- Scalar graphics, `PIXEL()` readback, framebuffer copy/merge.
- `FASTGFX CREATE/FPS/SWAP/SYNC/CLOSE`.

Useful variants:

```sh
# A narrower rerun while debugging display changes.
$PY porttools/pico_fs_vm_smoke.py display --port "$PORT"

# Use a different scratch flash slot.
$PY porttools/pico_fs_vm_smoke.py flash --port "$PORT" --flash-slot 3

# Require saved PSRAM option in the device suite.
$PY porttools/pico_fs_vm_smoke.py device --port "$PORT" --expect-psram
```

### Tier 2: Console and Input

Run these on every PicoCalc board with an attached keyboard/display assembly:

```sh
$PY porttools/pico_console_smoke.py --port "$PORT"
$PY porttools/pico_input_smoke.py --port "$PORT"
$PY porttools/pico_keymap_smoke.py --port "$PORT" --features pico
$PY porttools/device_datetime_smoke.py --port "$PORT"
```

What they cover:

- USB serial line editing and prompt recovery.
- Console control sequences and backspace/delete behavior.
- Keyboard input and keymap behavior.
- Device date/time command surface.

These cover a healthy keypad path. They do not simulate a dead or NACKing
keypad MCU. See "Fixture-only gaps" below.

### Tier 3: SD and B: Drive

Run with an SD card inserted:

```sh
$PY porttools/pico_sd_smoke.py --port "$PORT"
$PY porttools/pico_files_smoke.py --port "$PORT" --drive B: --bulk-count 128
```

What they cover:

- B: drive mount and free-space query.
- SD-backed file create/read/write/delete.
- Bulk file operations.
- DOS-style `B:/...` path handling.

If no card is inserted, do not claim the SD tier passed.

### Tier 4: PicoCalc Pin Reservation and PSRAM

Run on every PicoCalc board:

```sh
$PY porttools/pico_picocalc_pin_smoke.py --port "$PORT"
```

Expected non-PSRAM state:

- GP0 label contains `KEYPAD MCU UART`.
- GP1 label contains `KEYPAD MCU UART`.
- `SETPIN GP0,DOUT` and `SETPIN GP1,DOUT` fail.

On an RP2350 PicoCalc with PSRAM wired to GP0, configure and verify:

```sh
$PY porttools/basic_serial.py \
  --port "$PORT" \
  --cmd 'OPTION PSRAM PIN GP0' || true

$PY porttools/pico_picocalc_pin_smoke.py \
  --port "$PORT" \
  --expect-psram-gp0
```

Expected PSRAM-on-GP0 state:

- GP0 label contains `PSRAM CS`.
- GP1 label contains `KEYPAD MCU UART`.
- `OPTION LIST` contains `OPTION PSRAM PIN GP0`.
- `MM.INFO(PSRAM SIZE)` is nonzero.
- `SETPIN GP0,DOUT` and `SETPIN GP1,DOUT` fail.

Optional PSRAM march tests:

```sh
$PY porttools/basic_serial.py \
  --port "$PORT" \
  --timeout 120 \
  --cmd 'RAM TEST 1' \
  --expect 'RAM TEST OK'

$PY porttools/basic_serial.py \
  --port "$PORT" \
  --timeout 120 \
  --cmd 'RAM TEST NOCACHE 1' \
  --expect 'RAM TEST OK'
```

Use full `RAM TEST`, `RAM TEST ALL`, or `porttools/psram_smoke.py` when the
change directly touches PSRAM allocation, aliases, option persistence, or RAM
slots. Full-range tests can take much longer than ordinary command smokes.

### Tier 5: FASTGFX Memory and Recovery

Run on display-capable PicoCalc builds:

```sh
$PY porttools/pico_fastgfx_memory_smoke.py --port "$PORT"
```

What it covers:

- Deliberately pressures heap until `FASTGFX CREATE` fails.
- Verifies the failure returns to the prompt instead of wedging.
- Runs `NEW`.
- Verifies a clean `FASTGFX CREATE/CLOSE` succeeds afterward.
- Verifies heap recovers.

This is the regression smoke for partial FASTGFX allocation cleanup. It is
especially useful after graphics, allocator, VM heap, or PSRAM changes.

On small RP2040 heaps, a large user program may still be unable to allocate two
FASTGFX buffers. That is a program memory budget issue if the smoke proves
failure cleanup and subsequent clean create/close work.

### Tier 6: PIO DMA Regression Smoke

Run on Pico and PicoCalc boards after PIO, DMA, SDK, or RP2350 changes:

```sh
$PY porttools/pico_pio_dma_smoke.py --port "$PORT"
```

What it covers:

- `PIO INIT MACHINE` with the optional final `outout` argument.
- `PIO DMA RX` one-shot transfer.
- `PIO DMA TX` one-shot transfer.
- `MM.INFO(PIO RX DMA)` and `MM.INFO(PIO TX DMA)` busy polling.
- pio2 RX/TX DREQ mapping on RP2350.

Expected on RP2350: pio0 and pio2 both pass.

### Tier 7: Comprehensive PIO API Smoke

Run when validating PIO refactors, DMA changes, SDK compatibility work, or
anything that touches interrupt/teardown behavior:

```sh
$PY porttools/pico_pio_api_smoke.py --port "$PORT"
```

What it covers:

- PIO expression helpers:
  `PIO(PINCTRL ...)`, `PIO(EXECCTRL ...)`, `PIO(SHIFTCTRL ...)`,
  `PIO(FSTAT ...)`, `PIO(FDEBUG ...)`, `PIO(FLEVEL ...)`,
  `PIO(NEXT LINE ...)`.
- `PIO CONFIGURE`.
- PIO assembler directives and representative instruction classes.
- `PIO PROGRAM`, `PIO PROGRAM LINE`.
- `PIO START`, `PIO STOP`, `PIO EXECUTE`.
- `PIO WRITE`, `PIO READ`.
- RP2350 `PIO READFIFO`, `PIO WRITEFIFO`, and `PIO SET BASE`.
- `PIO DMA RX` and `PIO DMA TX` on pio0 and pio2.
- DMA transfer sizes 8, 16, and 32.
- Finite DMA ring arguments.
- Continuous DMA ring buffers with `PIO MAKE RING BUFFER`.
- `MM.INFO(PIO RX DMA POINTER)` and `MM.INFO(PIO TX DMA POINTER)`.
- DMA completion callbacks.
- PIO RX and TX FIFO interrupts.
- `NEW`/teardown after continuous chained DMA.

This smoke intentionally uses pinless state-machine programs so it is safe on
PicoCalc boards where GP0/GP1 are boot-reserved.

If this smoke wedges a board, treat it as a real firmware bug until proven
otherwise. It previously exposed RP2350 continuous TX ring DMA teardown getting
stuck when chained DMA channels were aborted without first disabling and
neutralizing `CHAIN_TO`.

### Tier 8: WiFi and Network Conformance

For PicoCalc WiFi boards, first connect:

```sh
$PY porttools/basic_serial.py \
  --port "$PORT" \
  --long-timeout 45 \
  --cmd 'WEB CONNECT "ssid","password"'
```

Do not commit real credentials to docs, scripts, or transcripts.

Quick web status:

```sh
$PY porttools/pico_fs_vm_smoke.py web \
  --port "$PORT" \
  --connect-command 'WEB CONNECT'
```

If credentials are not already saved, use a quoted command locally:

```sh
$PY porttools/pico_fs_vm_smoke.py web \
  --port "$PORT" \
  --connect-command 'WEB CONNECT "ssid","password"'
```

Full network conformance:

```sh
$PY porttools/network_conformance.py all \
  --port "$PORT" \
  --suite-timeout 240
```

Use explicit host addresses when automatic detection chooses the wrong
interface:

```sh
$PY porttools/network_conformance.py all \
  --port "$PORT" \
  --host 192.168.1.23 \
  --device-host 192.168.1.57 \
  --suite-timeout 240
```

What it covers:

- TCP client request and stream modes.
- TCP server request/read/path/send/close.
- UDP send/receive and message/address variables.
- TFTP read/write.
- Telnet console.
- NTP against a local deterministic responder.
- MQTT connect/subscribe/publish/unsubscribe/close.

Network conformance starts host-side services and is slower than the ordinary
Pico smoke. Run it for WiFi changes, socket changes, option persistence changes,
and release validation on WiFi variants.

## Suggested Full Pass by Board

### PicoCalc RP2040

```sh
cmake --build build_picocalc_rp2040 -j
# Flash build_picocalc_rp2040/PicoMite.uf2.
$PY porttools/pico_fs_vm_smoke.py all --port "$PORT" --long-timeout 60
$PY porttools/pico_console_smoke.py --port "$PORT"
$PY porttools/pico_input_smoke.py --port "$PORT"
$PY porttools/pico_keymap_smoke.py --port "$PORT" --features pico
$PY porttools/device_datetime_smoke.py --port "$PORT"
$PY porttools/pico_picocalc_pin_smoke.py --port "$PORT"
$PY porttools/pico_fastgfx_memory_smoke.py --port "$PORT"
$PY porttools/pico_pio_dma_smoke.py --port "$PORT"
$PY porttools/pico_pio_api_smoke.py --port "$PORT"
```

Add SD tests when a card is inserted:

```sh
$PY porttools/pico_sd_smoke.py --port "$PORT"
$PY porttools/pico_files_smoke.py --port "$PORT" --drive B: --bulk-count 128
```

### PicoCalc RP2350

```sh
cmake --build build_picocalc_rp2350 -j
# Flash build_picocalc_rp2350/PicoMite.uf2.
$PY porttools/pico_fs_vm_smoke.py all --port "$PORT" --long-timeout 60
$PY porttools/pico_console_smoke.py --port "$PORT"
$PY porttools/pico_input_smoke.py --port "$PORT"
$PY porttools/pico_keymap_smoke.py --port "$PORT" --features pico
$PY porttools/device_datetime_smoke.py --port "$PORT"
$PY porttools/pico_picocalc_pin_smoke.py --port "$PORT"
$PY porttools/pico_pio_dma_smoke.py --port "$PORT"
$PY porttools/pico_pio_api_smoke.py --port "$PORT"
```

Add SD tests when a card is inserted:

```sh
$PY porttools/pico_sd_smoke.py --port "$PORT"
$PY porttools/pico_files_smoke.py --port "$PORT" --drive B: --bulk-count 128
```

If PSRAM is installed on GP0:

```sh
$PY porttools/basic_serial.py --port "$PORT" --cmd 'OPTION PSRAM PIN GP0' || true
$PY porttools/pico_picocalc_pin_smoke.py --port "$PORT" --expect-psram-gp0
$PY porttools/basic_serial.py --port "$PORT" --timeout 120 --cmd 'RAM TEST 1' --expect 'RAM TEST OK'
```

### PicoCalc WiFi RP2040

```sh
cmake --build build_picocalc_wifi_rp2040 -j
# Flash build_picocalc_wifi_rp2040/PicoMite.uf2.
$PY porttools/pico_fs_vm_smoke.py all --port "$PORT" --long-timeout 60
$PY porttools/pico_console_smoke.py --port "$PORT"
$PY porttools/pico_input_smoke.py --port "$PORT"
$PY porttools/pico_keymap_smoke.py --port "$PORT" --features pico
$PY porttools/pico_picocalc_pin_smoke.py --port "$PORT"
$PY porttools/pico_pio_dma_smoke.py --port "$PORT"
$PY porttools/pico_pio_api_smoke.py --port "$PORT"
$PY porttools/network_conformance.py all --port "$PORT" --suite-timeout 240
```

Add SD tests when a card is inserted:

```sh
$PY porttools/pico_sd_smoke.py --port "$PORT"
$PY porttools/pico_files_smoke.py --port "$PORT" --drive B: --bulk-count 128
```

RP2040 WiFi has less free heap. If a large user program cannot allocate
FASTGFX buffers, confirm `pico_fastgfx_memory_smoke.py` passes before calling
it a firmware cleanup bug.

### PicoCalc WiFi RP2350

```sh
cmake --build build_picocalc_wifi_rp2350 -j
# Flash build_picocalc_wifi_rp2350/PicoMite.uf2.
$PY porttools/pico_fs_vm_smoke.py all --port "$PORT" --long-timeout 60
$PY porttools/pico_console_smoke.py --port "$PORT"
$PY porttools/pico_input_smoke.py --port "$PORT"
$PY porttools/pico_keymap_smoke.py --port "$PORT" --features pico
$PY porttools/device_datetime_smoke.py --port "$PORT"
$PY porttools/pico_picocalc_pin_smoke.py --port "$PORT"
$PY porttools/pico_pio_dma_smoke.py --port "$PORT"
$PY porttools/pico_pio_api_smoke.py --port "$PORT"
$PY porttools/pico_fastgfx_memory_smoke.py --port "$PORT"
$PY porttools/network_conformance.py all --port "$PORT" --suite-timeout 240
```

Add SD tests when a card is inserted:

```sh
$PY porttools/pico_sd_smoke.py --port "$PORT"
$PY porttools/pico_files_smoke.py --port "$PORT" --drive B: --bulk-count 128
```

If PSRAM is on GP0:

```sh
$PY porttools/basic_serial.py --port "$PORT" --cmd 'OPTION PSRAM PIN GP0' || true
$PY porttools/pico_picocalc_pin_smoke.py --port "$PORT" --expect-psram-gp0
```

## Interpreting Failures

### Prompt Timeout

First distinguish a BASIC error from a lost prompt:

```sh
$PY porttools/basic_serial.py --port "$PORT" --boot-wait 1 --cmd 'PRINT "ALIVE"'
```

If the serial device exists but sync captures no bytes, the firmware may be
hard-locked. Try `UPDATE FIRMWARE` only if the prompt is responsive. Otherwise
use BOOTSEL/manual reset or SWD.

### BOOTSEL After Flash

If an RP2350 board drops back to BOOTSEL immediately after flashing, inspect
flash layout before assuming the UF2 copy failed. Saved options must not overlap
the firmware image. See `docs/flash-layout-note.md`.

### FASTGFX OOM

An out-of-memory error is not automatically a failure. The failure is a hang,
heap leak, corrupted framebuffer state, or inability to cleanly create/close
FASTGFX after `NEW`.

Use:

```sh
$PY porttools/pico_fastgfx_memory_smoke.py --port "$PORT"
```

### PIO DMA Hang

A board wedged by `pico_pio_api_smoke.py` is significant. Continuous ring DMA
uses chained DMA channels and teardown paths that ordinary one-shot DMA does
not cover. Verify whether the board recovers to the prompt after power cycle,
then isolate with the PIO API smoke before reducing the test.

### WiFi Failures

Run a narrow suite before the full network pass:

```sh
$PY porttools/network_conformance.py tcp-client --port "$PORT"
$PY porttools/network_conformance.py udp --port "$PORT"
```

Use `--host` and `--device-host` on multi-interface Macs.

## Fixture-only Gaps

The following are not fully covered by ordinary PicoCalc smokes:

- Dead or NACKing PicoCalc keypad MCU backoff. The healthy input/keymap smokes
  prove normal input but do not hold the STM32 absent or wedged.
- Camera error-path cleanup. PicoCalc has no camera fixture; run on a
  camera-capable PicoMite target with the camera absent or faulted.
- Visual confirmation of some display quality issues. The display smokes check
  readback and command survival, not human-perceived rendering quality.

Document these as not run when reporting a PR smoke pass.

## Reporting Template

Use this shape in PR notes:

```text
Board: PicoCalcWiFi RP2350B
Firmware: <branch> @ <commit> + <dirty changes summary>
Flashed: build_picocalc_wifi_rp2350/PicoMite.uf2
Device ID: <MM.INFO$(ID)>
Options: <PSRAM/WiFi/SD relevant state>

Builds:
- picocalc_rp2040: pass
- picocalc_wifi_rp2040: pass
- picocalc_rp2350: pass
- picocalc_wifi_rp2350: pass

Hardware smokes:
- pico_fs_vm_smoke.py all: pass
- pico_console_smoke.py: pass
- pico_input_smoke.py: pass
- pico_keymap_smoke.py --features pico: pass
- device_datetime_smoke.py: pass
- pico_sd_smoke.py: pass/skip, reason
- pico_files_smoke.py --drive B: pass/skip, reason
- pico_picocalc_pin_smoke.py: pass
- pico_fastgfx_memory_smoke.py: pass
- pico_pio_dma_smoke.py: pass
- pico_pio_api_smoke.py: pass
- network_conformance.py all: pass/skip, reason

Known not run:
- keypad-MCU fault fixture
- camera error-path fixture
```
