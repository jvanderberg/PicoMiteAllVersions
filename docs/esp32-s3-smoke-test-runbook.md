# ESP32-S3 Smoke Test Runbook

This document is the hardware smoke-test checklist for ESP32-S3 builds. It is
intended for PR validation and regression triage on real boards, especially the
Freenove ESP32-S3 WROOM board with ILI9341 LCD, FT6336 touch, microSD, ES8311
audio, WiFi, and PSRAM.

Run commands from the repo root unless noted otherwise.

## Scope

The full ESP32-S3 smoke pass should answer six questions:

1. Does the ESP32-S3 firmware build and flash cleanly?
2. Does the board boot to a responsive MMBasic prompt with the expected profile?
3. Do A: flash, B: SD, saved flash slots, and VM/FRUN paths work end-to-end?
4. Do board peripherals still work after the SDK-compatibility refactor?
5. Do WiFi, TLS, MQTT, web transport, and telnet paths survive real network use?
6. Do risky teardown/error paths avoid hangs, hard faults, and stale state?

Host tests and compile-only gates are not a substitute for this runbook. The
ESP32 display, PSRAM, flash reset, audio codec, WiFi, TLS, and web-server paths
all have timing and hardware behavior that only shows up on a board.

## Requirements

- Python with `pyserial`; this repo has generally used `python3.11`.
- ESP-IDF environment for build and flash.
- A known serial port, for example `/dev/cu.usbmodem1101` on macOS.
- For Freenove tests, the Freenove board profile applied with `CONFIGURE FREENOVE`.
- For WiFi tests, local credentials provided through `WEB CONNECT`.
- For SD tests, an inserted microSD card mounted by the firmware as `B:`.
- For audio tests, speakers or headphones connected to the board output.
- Do not run USB keyboard smokes as part of the serial smoke pass; they can
  disrupt the smoke connection unless routed through telnet.
- Do not run LCD_CAM/VGA smokes on the Freenove board. Keep them for a board
  with the matching LCD_CAM/VGA fixture.

Useful shell setup:

```sh
export PORT=/dev/cu.usbmodem1101
export PY=python3.11
```

Quick prompt check:

```sh
$PY porttools/basic_serial.py \
  --port "$PORT" \
  --boot-wait 2 \
  --cmd 'PRINT MM.DEVICE$' \
  --cmd 'PRINT MM.INFO$(ID)'
```

## Build And Flash

Build the ESP32-S3 image:

```sh
./buildesp32.sh esp32_s3
```

Flash through USB Serial/JTAG:

```sh
. "$HOME/esp/esp-idf/export.sh"
idf.py -C ports/esp32_s3 -p "$PORT" flash
```

Use `erase_flash flash` only when intentionally wiping saved options and flash
slots:

```sh
idf.py -C ports/esp32_s3 -p "$PORT" erase_flash flash
```

After flashing, confirm the prompt, firmware identity, and PSRAM size:

```sh
$PY porttools/basic_serial.py \
  --port "$PORT" \
  --boot-wait 2 \
  --timeout 10 \
  --cmd 'PRINT "DEVICE="+MM.DEVICE$' \
  --cmd 'PRINT "VER="+MM.VER' \
  --cmd 'PRINT "ID="+MM.INFO$(ID)' \
  --cmd 'PRINT "PSRAM="+STR$(MM.INFO(PSRAM SIZE))'
```

Do not continue with board-specific smokes if the reported device or profile is
wrong.

## Freenove Profile

Apply the Freenove profile after a fresh erase or when the board boots with a
generic profile:

```sh
$PY porttools/basic_serial.py \
  --port "$PORT" \
  --timeout 10 \
  --long-timeout 20 \
  --cmd 'CONFIGURE FREENOVE'
```

Then verify the live board options:

```sh
$PY porttools/basic_serial.py \
  --port "$PORT" \
  --boot-wait 2 \
  --cmd 'PRINT "HRES=" + STR$(MM.HRES)' \
  --cmd 'PRINT "VRES=" + STR$(MM.VRES)' \
  --cmd 'PRINT MM.INFO$(AUDIO)' \
  --cmd 'PRINT "PSRAM=" + STR$(MM.INFO(PSRAM SIZE))' \
  --cmd 'OPTION LIST'
```

Expected Freenove state:

- `MM.DEVICE$` reports an ESP32-S3 Freenove ILI9341 build.
- `MM.HRES=320` and `MM.VRES=240`.
- `OPTION AUDIO ES8311 ...` is present.
- `OPTION LCDPANEL ILI9341 ...` is present.
- `OPTION TOUCH FT6336 ...` is present.
- `OPTION SYSTEM I2C ...` and `OPTION SDCARD ...` are present.
- `MM.INFO(PSRAM SIZE)` is nonzero. On current Freenove hardware, the exposed
  bitmap capacity is 6291456 bytes.

If options list the Freenove LCD but `MM.HRES` and `MM.VRES` are `0`, rerun
`CONFIGURE FREENOVE` before display, GUI, or touch smokes.

## Smoke Tiers

### Tier 0: Prompt And Device Identity

Run this after every flash or reset:

```sh
$PY porttools/basic_serial.py \
  --port "$PORT" \
  --boot-wait 2 \
  --cmd 'PRINT "DEVICE="+MM.DEVICE$' \
  --cmd 'PRINT "ID="+MM.INFO$(ID)' \
  --cmd 'PRINT "HEAP="+STR$(MM.INFO(HEAP))' \
  --cmd 'PRINT "PSRAM="+STR$(MM.INFO(PSRAM SIZE))'
```

### Tier 1: Core Filesystem, VM, And GPIO

Run the default smoke first:

```sh
$PY porttools/esp32_fs_vm_smoke.py all \
  --port "$PORT" \
  --long-timeout 60
```

What it covers:

- Prompt sync and `MM.INFO` sanity checks.
- B:/SD detection when a card is present.
- A: filesystem create/read/write/append/copy/rename/kill behavior.
- `LOAD`, `SAVE`, `RUN`, and `FRUN`.
- `FRUN` bytecode VM execution.
- Bridged `DIM` and VM runtime paths.
- Sieve benchmark.
- GPIO `DOUT`, `DIN`, and `ARAW`.
- PWM/SERVO on a non-reserved LEDC channel.

Useful focused reruns:

```sh
$PY porttools/esp32_fs_vm_smoke.py vm --port "$PORT"
$PY porttools/esp32_fs_vm_smoke.py gpio --port "$PORT"
```

### Tier 2: Display, FASTGFX, And VM Error Recovery

Run display and FASTGFX surface checks:

```sh
$PY porttools/esp32_display_surface_smoke.py \
  --port "$PORT" \
  --long-timeout 180
```

Run the Freenove display primitive performance gate separately after confirming
the LCD is live:

```sh
$PY porttools/esp32_fs_vm_smoke.py display-perf \
  --port "$PORT" \
  --long-timeout 180
```

When validating VM/FASTGFX error recovery, keep these repro files on A: or run
equivalent programs through the smoke harness:

```basic
' A:/fg_err_close.bas
OPTION EXPLICIT
ON ERROR SKIP : FASTGFX CLOSE
PRINT "OK_ERR_CLOSE"
```

```basic
' A:/vm_err_skip.bas
OPTION EXPLICIT
DIM INTEGER A%(1)
ON ERROR SKIP : A%(2)=7
ON ERROR CLEAR
PRINT "OK_VM_ERR_SKIP"
```

Expected result:

```basic
FRUN "A:/fg_err_close.bas"  ' prints OK_ERR_CLOSE
FRUN "A:/vm_err_skip.bas"   ' prints OK_VM_ERR_SKIP
```

These catch stale `ON ERROR SKIP`/`ErrNext` state crossing VM/native command
boundaries. A reset, hang, or missing prompt is a failure.

### Tier 3: Persistent Storage, Flash Slots, And PSRAM

Run flash slot and PSRAM checks:

```sh
$PY porttools/esp32_fs_vm_smoke.py flash psram \
  --port "$PORT" \
  --long-timeout 180
```

Add `--var-save` when validating `VAR SAVE` and `VAR RESTORE` explicitly:

```sh
$PY porttools/esp32_fs_vm_smoke.py flash psram \
  --var-save \
  --port "$PORT" \
  --long-timeout 180
```

Run a quick built-in PSRAM march:

```sh
$PY porttools/basic_serial.py \
  --port "$PORT" \
  --timeout 120 \
  --cmd 'RAM TEST 4' \
  --expect 'RAM TEST OK'
```

Run the comprehensive PSRAM harness when the change touches allocation,
framebuffers, RAM files, or PSRAM option persistence:

```sh
$PY porttools/psram_smoke.py --target esp32 \
  --port "$PORT" \
  --long-timeout 120 \
  --very-long-timeout 240
```

ESP32 expected skips are `RAM FILE LOAD` unsupported, BASIC `DIM` allocations
using the internal heap, and the unreachable "PSRAM not enabled" path when the
board has working PSRAM.

### Tier 4: Audio

Run the command-level audio smoke:

```sh
$PY porttools/esp32_audio_smoke.py --port "$PORT"
```

What it covers:

- Audio option presence.
- ES8311 status reporting on Freenove.
- `PLAY VOLUME`.
- `PLAY TONE`.
- `PLAY SOUND`.
- `PLAY NOTE`.
- `PLAY STOP`.

For a human audible check, run:

```sh
$PY porttools/basic_serial.py \
  --port "$PORT" \
  --timeout 10 \
  --long-timeout 20 \
  --cmd 'PLAY STOP' \
  --cmd 'PLAY VOLUME 25,25' \
  --cmd 'PLAY TONE 880,880,2000' \
  --cmd 'PAUSE 2200' \
  --cmd 'PLAY TONE 440,660,1200' \
  --cmd 'PAUSE 1400' \
  --cmd 'PLAY SOUND 1,B,S,523,20' \
  --cmd 'PLAY SOUND 2,B,S,659,20' \
  --cmd 'PLAY SOUND 3,B,S,784,20' \
  --cmd 'PAUSE 1200' \
  --cmd 'PLAY STOP'
```

The automated command smoke passing is not enough for Freenove validation. The
audible output must be heard at least once after a profile or audio-driver
change.

### Tier 5: GUI, Touch, And SD

Run the full GUI control wrapper after the LCD is live:

```sh
$PY porttools/esp32_gui_controls_smoke.py \
  --target "$PORT" \
  --cleanup \
  --long-timeout 120
```

This covers basic controls, text/numeric controls, console edit paths, gauges,
areas, and message box setup under both `RUN` and `FRUN`.

Run SD smoke when a card is present:

```sh
$PY porttools/esp32_sd_smoke.py \
  --port "$PORT" \
  --long-timeout 60
```

If no card is inserted, do not claim the SD tier passed.

### Tier 6: WiFi, TLS, MQTT, And Network Conformance

Run TLS scan and secure MQTT checks:

```sh
$PY porttools/esp32_tls_scan_smoke.py \
  --port "$PORT" \
  --long-timeout 90 \
  --connect-command 'WEB CONNECT "SSID","PASSWORD"'
```

What it covers:

- WiFi connect.
- Web scan array mode.
- Web scan text mode.
- TLS certificate load.
- TLS peer request and response.
- TLS stream peer request and response.
- TLS MQTT connect, subscribe, receive, and publish.

Run full network conformance:

```sh
$PY porttools/network_conformance.py all \
  --port "$PORT" \
  --long-timeout 90 \
  --connect-command 'WEB CONNECT "SSID","PASSWORD"'
```

What it covers:

- TCP client.
- TCP server.
- `WEB TRANSMIT` page/file/css/js/image/code.
- UDP.
- TFTP.
- Telnet console.
- NTP.
- MQTT.

The older chained form remains useful when validating the fs/vm harness itself:

```sh
$PY porttools/esp32_fs_vm_smoke.py network \
  --run-network \
  --port "$PORT" \
  --long-timeout 60 \
  --network-suite-timeout 240 \
  --connect-command 'WEB CONNECT "SSID","PASSWORD"'
```

### Tier 7: Web Console

Enable and test the reserved web console endpoint:

```sh
$PY porttools/basic_serial.py \
  --port "$PORT" \
  --timeout 10 \
  --long-timeout 60 \
  --cmd 'WEB CONNECT "SSID","PASSWORD"' \
  --cmd 'OPTION WEB CONSOLE ON' \
  --cmd 'PRINT "IP=" + MM.INFO$(IP ADDRESS)' \
  --cmd 'PRINT "PORT=" + STR$(MM.INFO(TCP PORT))'
```

Then use the printed IP address:

```sh
$PY porttools/esp32_web_console_smoke.py \
  --host 192.168.4.44 \
  --serial-port "$PORT" \
  --timeout 30 \
  --display-sequence \
  --keyboard-sequence \
  --editor-scroll-stress
```

If the web console smoke reports connection refused, first verify
`MM.INFO$(IP ADDRESS)` is non-empty and `OPTION WEB CONSOLE ON` has been set.

### Tier 8: LCD_CAM/VGA Fixture

Do not run this on the Freenove board. Save it for the ESP32-S3 board with a
matching LCD_CAM/VGA fixture:

```sh
$PY porttools/esp32_lcdcam_vga_smoke.py \
  --port "$PORT" \
  --long-timeout 180
```

## Suggested Full Pass By Board

Freenove ESP32-S3:

```sh
./buildesp32.sh esp32_s3
idf.py -C ports/esp32_s3 -p "$PORT" flash
$PY porttools/basic_serial.py --port "$PORT" --cmd 'CONFIGURE FREENOVE'
$PY porttools/esp32_fs_vm_smoke.py all --port "$PORT" --long-timeout 60
$PY porttools/esp32_display_surface_smoke.py --port "$PORT" --long-timeout 180
$PY porttools/esp32_fs_vm_smoke.py psram display-perf flash --port "$PORT" --long-timeout 180
$PY porttools/esp32_audio_smoke.py --port "$PORT"
$PY porttools/esp32_gui_controls_smoke.py --target "$PORT" --cleanup --long-timeout 120
$PY porttools/esp32_sd_smoke.py --port "$PORT" --long-timeout 60
$PY porttools/esp32_tls_scan_smoke.py --port "$PORT" --long-timeout 90 --connect-command 'WEB CONNECT "SSID","PASSWORD"'
$PY porttools/network_conformance.py all --port "$PORT" --long-timeout 90 --connect-command 'WEB CONNECT "SSID","PASSWORD"'
```

Generic ESP32-S3 without Freenove peripherals:

```sh
./buildesp32.sh esp32_s3
idf.py -C ports/esp32_s3 -p "$PORT" flash
$PY porttools/esp32_fs_vm_smoke.py all --port "$PORT" --long-timeout 60
$PY porttools/esp32_fs_vm_smoke.py psram flash --port "$PORT" --long-timeout 180
$PY porttools/esp32_tls_scan_smoke.py --port "$PORT" --long-timeout 90 --connect-command 'WEB CONNECT "SSID","PASSWORD"'
$PY porttools/network_conformance.py all --port "$PORT" --long-timeout 90 --connect-command 'WEB CONNECT "SSID","PASSWORD"'
```

LCD_CAM/VGA board:

```sh
./buildesp32.sh esp32_s3
idf.py -C ports/esp32_s3 -p "$PORT" flash
$PY porttools/esp32_fs_vm_smoke.py all --port "$PORT" --long-timeout 60
$PY porttools/esp32_lcdcam_vga_smoke.py --port "$PORT" --long-timeout 180
```

## Interpreting Failures

Wrong or generic profile:

- Run `OPTION LIST`.
- If Freenove options are missing, run `CONFIGURE FREENOVE`.
- If options exist but LCD geometry is zero, run `CONFIGURE FREENOVE` again and
  retry the display tier.

Prompt disappears after `FLASH SAVE`:

- The flash smoke should use `CPU RESTART` for reset/resync.
- If the board is manually reset with RTS and comes back with a generic profile,
  use `CPU RESTART` or reapply `CONFIGURE FREENOVE` before continuing.

FASTGFX or `ON ERROR SKIP` hangs:

- Rerun the two Tier 2 repro programs with `FRUN`.
- A reset or missing prompt indicates stale VM/native error state.

Audio command smoke passes but nothing is heard:

- Verify `OPTION AUDIO ES8311 ...` is present.
- Run the audible Tier 4 sequence.
- Confirm volume, speaker/headphone connection, and amp polarity before
  accepting the audio tier.

WiFi failures:

- Do not record credentials in logs or documentation.
- Confirm `WEB CONNECT` succeeds and `MM.INFO$(IP ADDRESS)` is non-empty.
- Run TLS scan before full conformance when isolating certificate or MQTT
  failures.

NTP failures:

- Check whether the suite received any valid time response before treating a
  display-format mismatch as a firmware failure.

USB keyboard:

- Leave out of the serial smoke pass unless the board is accessed over telnet.

Quad SPI and LCD_CAM:

- Quad mode is treated as an implementation detail unless a board fixture
  exposes it directly.
- LCD_CAM/VGA is documented here but should be run only on the next matching
  board, not on Freenove.

## 2026-06-11 Freenove PR-42 Result

Board: Freenove ESP32-S3 WROOM with ILI9341 LCD, FT6336 touch, microSD, ES8311
audio, WiFi, and PSRAM.

Build: `jv/sdk-compat-retirement @ d7d01f7` plus PR-42 smoke-test working tree
fixes.

Flashed image:

```text
ports/esp32_s3 build from ./buildesp32.sh esp32_s3
```

Verified live profile:

```text
MMBasic ESP32-S3 Freenove ILI9341
MM.HRES=320
MM.VRES=240
OPTION AUDIO ES8311 ...
OPTION LCDPANEL ILI9341 ...
OPTION TOUCH FT6336 ...
OPTION SDCARD ...
PSRAM SIZE=6291456
```

Passed on hardware:

- Host native regression suite: 292 passed, 0 failed.
- `esp32_fs_vm_smoke.py all`.
- `esp32_display_surface_smoke.py`.
- `esp32_fs_vm_smoke.py psram display-perf flash`.
- `RAM TEST 4`.
- `esp32_audio_smoke.py`.
- Audible Freenove audio verification.
- `esp32_tls_scan_smoke.py`.
- `network_conformance.py all`.
- `FRUN "A:/fg_err_close.bas"` stale error-path repro.
- `FRUN "A:/vm_err_skip.bas"` VM `ON ERROR SKIP` repro.

Known intentionally unrun in this Freenove pass:

- USB keyboard smoke.
- LCD_CAM/VGA smoke.
- Quad fixture-specific validation.
