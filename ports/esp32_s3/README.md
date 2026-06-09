# MMBasic Anywhere - ESP32-S3

**Download:** prebuilt ESP32-S3 firmware is published on the [latest release](https://github.com/jvanderberg/PicoMiteAllVersions/releases/tag/latest). **Two images are published — pick by your board's PSRAM line mode**, which is a compile-time choice (one image cannot serve both):

- `MMBasic-Anywhere-esp32-s3-octal-merged.bin` — for boards with **octal PSRAM** (the `R8` and `R16` modules, e.g. N16R8, N32R16V).
- `MMBasic-Anywhere-esp32-s3-quad-merged.bin` — for boards with **quad PSRAM** (the `R2` modules). A board with no PSRAM works on either image.

Not sure which? Neither image bricks a board — flash either and check `MM.INFO(PSRAM SIZE)`; if it returns `0`, the PSRAM line mode is wrong, so flash the other image. A board with no PSRAM still boots on the quad image (using only the internal heap). Flash the merged image to `0x0` **in DIO mode**; see [Flash](#flash). Background: [docs/real-hal/esp32-s3-quad-port.md](../../docs/real-hal/esp32-s3-quad-port.md). Building from source is only needed for development.

ESP32-S3 port with selectable board profiles. The generic profile boots over USB Serial/JTAG without assuming board peripherals; the Metro profile keeps the Adafruit Metro ESP32-S3 (#5500) N16R8 wiring used for bring-up. MMBasic claims the board's PSRAM at boot — as much as the chip actually has — for its program/variable heap; `PSRAMsize` and the shared `RAM` command surface match Pico variants. The port can use the ESP32-S3 native USB port either as a USB Serial/JTAG console or as a USB HID host for an external keyboard.

Plan: [docs/real-hal/esp32-s3-port.md](../../docs/real-hal/esp32-s3-port.md). Session log: [docs/real-hal/esp32-s3-port-log.md](../../docs/real-hal/esp32-s3-port-log.md).

## Prerequisites

- ESP-IDF 5.3 installed at `~/esp/esp-idf/`.
- ESP32-S3 board connected over USB-C. The Metro profile is currently verified
  on the Adafruit Metro ESP32-S3 N16R8.
- `dfu-util` for recovery paths.
- `pyserial` if using `probe.py`.

Load the ESP-IDF environment before building:

```sh
. ~/esp/esp-idf/export.sh
```

## Build

There are two ESP32-S3 ports, differing only in PSRAM line mode (they share
`main/sources.cmake` verbatim; the heap is sized at boot to whatever PSRAM the
chip has):

- `ports/esp32_s3` — octal PSRAM (R8 / R16).
- `ports/esp32_s3_quad` — quad PSRAM (R2) / no-PSRAM.

Each is a self-contained ESP-IDF project. Build either the standard way:

```sh
cd ports/esp32_s3        # or ports/esp32_s3_quad
idf.py set-target esp32s3   # one-time, or after idf.py fullclean
idf.py build
```

The firmware image is written under that port's `build/`. From the repo root,
the opt-in helper runs the HAL purity gate first and then builds a port (or
`all` for both):

```sh
./buildesp32.sh esp32_s3          # octal
./buildesp32.sh esp32_s3_quad     # quad
./buildesp32.sh all               # both
```

## Flash

### Flash a release build

Download the merged image for your board (octal or quad — see [Download](#mmbasic-anywhere---esp32-s3)) from the [latest release](https://github.com/jvanderberg/PicoMiteAllVersions/releases/tag/latest) and flash with [`esptool`](https://github.com/espressif/esptool) (`pip install esptool`) — no ESP-IDF checkout needed. Replace the port in every command below with your device's node: `/dev/cu.usbmodem*` on macOS/Linux, or `COMx` on Windows (find the number under **Device Manager → Ports (COM & LPT)**; a native USB Serial/JTAG board enumerates without an extra driver, but a board with a CP210x/CH34x USB-UART bridge needs that vendor driver first). Substitute `quad` for `octal` in the filename if that's your board.

Erase first, then write the merged image at `0x0` — a bare reflash leaves the A: filesystem and `FLASH SAVE` slots untouched, so erasing guarantees a clean, deterministic state (see [Does skipping the erase matter?](#does-skipping-the-erase-matter)):

```sh
esptool --chip esp32s3 -p /dev/cu.usbmodem* erase_flash
esptool --chip esp32s3 -p /dev/cu.usbmodem* write_flash 0x0 MMBasic-Anywhere-esp32-s3-octal-merged.bin
```

**Flash in DIO mode.** The merged image already carries a DIO header, so the
command above is correct. If you flash with the Espressif **Flash Download
Tool** GUI instead, set **SPI MODE = DIO** — its default of QIO will boot-loop
an octal-PSRAM (R8) board (`TG0WDT` / `ets_loader.c`), because QIO conflicts
with the octal PSRAM lines.

A clean flash boots the `GENERIC` profile over USB Serial/JTAG; see [Board Profiles](#board-profiles) to select your board. If the prompt never appears but PSRAM was detected, the board may route its USB-C only to a UART bridge rather than the chip's native USB — confirm you're on the native-USB connector.

If the connect hangs (`Connecting...` on macOS/Linux, or a timeout on Windows), put the board into ROM USB Direct mode and retry: hold BOOT, press and release RESET, release BOOT, then re-run the command once the port re-enumerates. This is a USB CDC binding quirk, not a firmware build problem. After flashing, the port may re-enumerate under a new name (`COMx` number / `usbmodem*` suffix) as the board boots firmware rather than the ROM bootloader.

### Does skipping the erase matter?

It can. Flashing the merged (or split) image overwrites only the range it spans — bootloader, partition table, NVS, and the app (`0x0`–`~0x1e6000`). The two data partitions that live above the app are **not** touched:

- `lfsdata` (the A: LittleFS drive) at `0x210000`
- `mmslots` (`FLASH SAVE` / `VAR SAVE`) at `0x2f0000`

Reflashing the *same* firmware lineage without erasing is usually fine — and even handy, since your A: files and saved slots survive. The hazard is **partition-table drift**: if you move to a build whose partition map differs, or come from foreign firmware (Arduino/MicroPython), the new partition table points A: and the slots at bytes laid out for the old map. LittleFS may fail to mount or look corrupt and `FLASH LOAD` can read garbage. `erase_flash` removes that ambiguity by blanking those regions so the firmware re-initializes them. When in doubt — version changes, switching firmware, or anything acting weird — erase first.

### Flash from source

With ESP-IDF loaded and the port built ([Build](#build)):

```sh
idf.py -p /dev/cu.usbmodem* flash
```

If the connect hangs, use the same ROM USB Direct recovery as the release path above (hold BOOT, tap RESET, release BOOT, retry).

## Monitor And Probe

### Interactive REPL

The console is a full MMBasic REPL over the native USB Serial/JTAG interface
(115200 baud, nominal — it is USB CDC, not a real UART). For an interactive
terminal, use the `/dev/cu.*` node (not `/dev/tty.*` — `cu` does not assert DTR
on open, which avoids resetting the board into the bootloader):

```sh
screen /dev/cu.usbmodem* 115200      # exit: Ctrl-A then K, then y
```

`minicom -D /dev/cu.usbmodem* -b 115200` and `tio /dev/cu.usbmodem*` also work.
Only one program can hold the port at a time — close the terminal before running
`probe.py` / the smoke suites, and vice versa.

### Monitor and one-shot commands

Standard ESP-IDF monitor (firmware logs + console):

```sh
idf.py -p /dev/cu.usbmodem* monitor
```

Exit with Ctrl-]. For scripted single commands (no terminal):

```sh
python3 probe.py /dev/cu.usbmodem* --cmd 'PRINT MM.VER'
```

`probe.py` avoids the DTR pulse behavior that can reset the Metro into the wrong USB mode. Terminal programs such as `picocom` can work, but be careful with DTR/HUPCL reset behavior on open.

## WiFi, Telnet, And Web Console

WiFi credentials are stored in NVS. `OPTION WIFI` connects immediately, and the saved credentials are reused on later boots when network services are enabled.

```basic
OPTION WIFI "your-ssid","your-password"
PRINT MM.INFO$(IP ADDRESS)
```

Common network console setup:

```basic
OPTION TCP SERVER PORT 80
OPTION TELNET CONSOLE ON
OPTION WEB CONSOLE ON
CPU RESTART
```

`OPTION TELNET CONSOLE ON` mirrors console I/O to telnet while keeping the local console active. `OPTION TELNET CONSOLE ONLY` makes telnet the only console. `OPTION TELNET CONSOLE OFF` disables telnet console use.

The web console listens on `Option.TCP_PORT` (default **80**) and uses this path:

```
http://<device-ip>/__web_console/
```

The root `/` remains available for BASIC programs serving pages with `WEB TRANSMIT PAGE`.

## USB Console And Keyboard

The native USB-C port has two saved runtime roles:

| Command | USB role after reboot |
|---|---|
| `OPTION USB SERIAL` | USB Serial/JTAG console for a host computer |
| `OPTION USB KEYBOARD` | USB host mode for a HID boot keyboard |

Changing the USB role saves the option and reboots. `LIST OPTIONS` prints the saved role.

```basic
OPTION USB SERIAL
OPTION USB KEYBOARD
```

In keyboard mode, connect a USB keyboard to the Metro USB-C port through a suitable host cable or adapter. Local VGA remains the display console. Key repeat uses the standard MMBasic default timing: 600 ms before repeat starts, then 150 ms between repeated keys.

Holding BOOT during reset forces USB Serial/JTAG for that boot without changing the saved USB role.

## Board Profiles

One firmware image serves every supported board. A board profile carries the default wiring for that board's LCD, SD card, touch, and audio. The active profile is saved and reapplied on every boot. Select one with the standard `CONFIGURE` command (an alias for `OPTION RESET`); it resets options to that board's defaults, saves, and reboots:

```basic
CONFIGURE LIST       ' GENERIC / METRO / FREENOVE ILI9341
CONFIGURE FREENOVE   ' apply Freenove ILI9341 defaults and restart
CONFIGURE GENERIC    ' conservative defaults, no board peripherals
```

`MM.INFO$(DEVICE)` reports the active board (e.g. `MMBasic ESP32-S3 Freenove ILI9341`). `OPTION PLATFORM` is unrelated to board selection — it sets a free-text label returned by `MM.INFO$(PLATFORM)`, exactly as on the PicoMite.

### Supported platforms

| Profile | Hardware | What the profile wires up |
|---|---|---|
| `GENERIC` | Any ESP32-S3 dev board | USB Serial/JTAG console, `A:` (LittleFS), WiFi, PSRAM. No display, SD, touch, or audio assumed. |
| `METRO` | Adafruit Metro ESP32-S3 (#5500) N16R8 | SPI microSD, I2S audio DAC (BCLK GP5 / WS GP6 / DOUT GP7), WS2812. The board used for VGA bring-up. |
| `FREENOVE ILI9341` | Freenove FNK0104A/B 2.8" | ILI9341 SPI LCD (SCLK 12 / MOSI 11 / MISO 13 / CS 10 / DC 46 / BL 45), FT6336U capacitive touch, ES8311 audio codec, microSD socket (SCLK 38 / MOSI 40 / MISO 39 / CS 47). |

### Generic bring-up

`GENERIC` is the safe default on a clean flash and the starting point for any board. It brings up only what every ESP32-S3 has — the USB Serial/JTAG REPL, the `A:` LittleFS filesystem, WiFi/network, and PSRAM if present — and reserves **no** GPIO for a display, SD bus, or audio, so it runs on any board without pin conflicts. Bring a new board up on `GENERIC` first, confirm the REPL and `A:` work, then layer on peripherals.

### Configuring an unsupported board

For a board that has no profile, run on `GENERIC` and configure peripherals by hand. The LCD, touch, SD card, audio, and VGA are all `OPTION`-driven and work on any board, exactly as on the other PicoMite ports — a board profile only supplies these as defaults:

```basic
OPTION SYSTEM SPI GP12,GP11,GP13         ' LCD SPI bus: clk, mosi, miso
OPTION LCDPANEL ILI9341, LANDSCAPE, GP46, 0, GP10, GP45, INVERT
                                         ' controller, orientation, DC, RST (0 = none), CS [, BL] [, INVERT]
OPTION SYSTEM I2C GP16,GP15              ' shared I2C bus: sda, scl [, SLOW]
OPTION TOUCH FT6336, GP17, GP18          ' FT6336U: irq, reset [, click] [, threshold]
OPTION SDCARD GP47,GP38,GP40,GP39        ' dedicated-SPI SD: cs, clk, mosi, miso
OPTION AUDIO I2S GP5,GP6,GP7             ' external I2S DAC — see Audio below
OPTION VGA 3BIT GP8,GP9,GP10,GP11,GP12   ' resistor-DAC VGA — see VGA below
CPU RESTART
```

`OPTION SDCARD cs, clk, mosi, miso` wires a dedicated-SPI SD card and mounts it as `B:`; the four pins are saved and shown in `LIST OPTIONS`. (On the built-in profiles these same pins are seeded automatically — e.g. `CONFIGURE FREENOVE` sets the Freenove socket, display, and touch pins for you.)

`OPTION SYSTEM SPI` assigns the dedicated LCD SPI bus and `OPTION LCDPANEL` picks the controller and control pins — set the bus first, then the panel. ILI9341 (320×240, landscape) is the supported controller. Touch follows the same bus-then-device pattern, with the PicoMite syntax: `OPTION SYSTEM I2C` declares the bus, `OPTION TOUCH FT6336` attaches the capacitive controller, and `OPTION TOUCH CALIBRATE` adjusts its mapping. Each setter validates the pins, saves, and reboots; the matching `... DISABLE` form clears it.

## VGA

The ESP32-S3 VGA output uses LCD_CAM RGB scanout into a resistor-DAC style VGA input. No external pixel clock pin is routed or required.

For the VGA Serial Wombat 3-bit board, wire one bit per channel plus sync:

| VGA signal | Metro GPIO |
|---|---|
| Red | GP8 |
| Green | GP9 |
| Blue | GP10 |
| HSync | GP11 |
| VSync | GP12 |

Recommended option set:

```basic
OPTION VGA 3BIT GP8,GP9,GP10,GP11,GP12
OPTION VGA SYNC NEGATIVE,NEGATIVE
OPTION VGA CLOCK 25MHZ
OPTION VGA DRIVE 2
CPU RESTART
```

Available display modes:

| Command | Logical framebuffer | Scanout |
|---|---|---|
| `MODE 1` | 640 x 480 RGB332 | 640 x 480 VGA |
| `MODE 2` | 320 x 240 RGB332 | 2x scaled to 640 x 480 |
| `MODE 3` | 320 x 240 RGB332 | 2x scaled with 3-bit output dithering |

`OPTION VGA DISABLE` clears the VGA configuration and reboots. Full RGB332 wiring is also supported with `OPTION VGA r2,r1,r0,g2,g1,g0,b1,b0,hsync,vsync`.

## Audio

`PLAY TONE`, `PLAY SOUND` (all waveforms S/Q/T/W/P/N/U, the four SOUND slots),
and `PLAY NOTE` are synthesized by the shared software synthesizer
(`shared/audio/synth_pcm.c`, the same kernel the RP2 ports use). The ESP32
backend can output either to an external standard-I2S DAC or to the ESP32-S3
I2S PDM TX DAC-style two-line output.

Default audio is I2S using the pins in `port_config.h` (defaults avoid the
strapping, USB, and Octal-PSRAM GPIOs on the N16R8):

| Signal | Default GPIO | `port_config.h` macro |
|---|---|---|
| BCLK (bit clock) | 5 | `HAL_PORT_AUDIO_I2S_BCLK_PIN` |
| WS / LRCLK | 6 | `HAL_PORT_AUDIO_I2S_WS_PIN` |
| DOUT (serial data) | 7 | `HAL_PORT_AUDIO_I2S_DOUT_PIN` |

Recommended I2S option set for the current Metro VGA wiring:

```basic
OPTION AUDIO I2S GP5,GP6,GP7
```

Audio configuration commands:

| Command | Backend | Pins |
|---|---|---|
| `OPTION AUDIO I2S bclk,data` | Standard I2S PCM for an external DAC/amp | Legacy two-pin form: `bclk`, inferred `ws = bclk + 1`, `data` |
| `OPTION AUDIO I2S bclk,ws,data` | Standard I2S PCM for an external DAC/amp | Explicit wiring form; `ws` must be `bclk + 1` |
| `OPTION AUDIO ES8311 bclk,ws,dout [,mclk [,ampen [,AMPLOW]]]` | ES8311 codec (I2S + I2C control) | Control bus from `OPTION SYSTEM I2C`; `mclk`/`ampen` accept 0 = none; `AMPLOW` = active-low amp enable |
| `OPTION AUDIO left,right` | ESP32-S3 I2S PDM TX DAC-style two-line output | left PDM output, right PDM output |
| `OPTION AUDIO PDM left,right` | Same as the bare two-pin form | left PDM output, right PDM output |
| `OPTION AUDIO DISABLE` | Audio off | none |

```basic
OPTION AUDIO I2S GP5,GP7
OPTION AUDIO I2S GP5,GP6,GP7
OPTION AUDIO ES8311 GP5,GP7,GP8,GP4,GP1,AMPLOW   ' the Freenove wiring (CONFIGURE FREENOVE seeds this)
OPTION AUDIO GP12,GP13
OPTION AUDIO PDM GP12,GP13
OPTION AUDIO DISABLE
PRINT MM.INFO$(AUDIO)
PRINT MM.INFO$(AUDIO STATUS)
```

For I2S, the two-pin form infers WS/LRCLK as `BCLK + 1`. `LIST OPTIONS` prints BCLK, WS/LRCLK, and DOUT. The PDM form configures the ESP32-S3 hardware PDM TX converter; the selected pins are the left/right PDM data outputs. `MM.INFO$(AUDIO)` reports `I2S`, `PDM`, or `OFF`. `MM.INFO$(AUDIO STATUS)` reports the active pins and I2S driver status.

PDM output can be filtered and amplified externally for analog audio. In
practice it also works directly into some high-impedance AUX inputs, but a
simple low-pass filter and amplifier remain the recommended hardware path.

Tone/SOUND synthesis runs at `HAL_PORT_AUDIO_SAMPLE_RATE` (44100). File playback
for `PLAY WAV`, `PLAY FLAC`, `PLAY MP3`, and `PLAY MODFILE` uses the shared
decoder path in `shared/audio/audio_stream.c`, feeding the selected backend with
16-bit stereo PCM. MIDI, ARRAY, and STREAM playback are not wired in this port.
The exhaustive `demos/sound/test_audio.bas` exercises the tone/SOUND surface.

## Current Status

Working on hardware:

- Interactive REPL over USB Serial/JTAG.
- `PRINT`, `FOR`/`NEXT`, `IF`/`ELSE`, `GOTO`/`GOSUB`, `LIST`, `EDIT`, `CLS`, `COLOUR`, and `CPU RESTART`.
- A: drive backed by LittleFS over an ESP-IDF flash partition.
- Bundled demos seeded to A:: `hello.bas`, `fizzbuzz.bas`, `sieve.bas`, and `mand.bas`.
- `FILES`, `LOAD`, `SAVE "file.bas"`, `RUN`, and `FRUN` for A: files.
- `RUN "mand.bas"` and `FRUN "mand.bas"` produce checksum `552868`; `FRUN` is currently about 24x faster on the Metro.
- B: drive rejects cleanly as not configured.
- `OPTION` persistence is backed by ESP-IDF NVS and has been hardware-smoked across reset/reflash.
- Default terminal colours survive errors and prompt recovery through shared MMBasic colour-state restoration.
- `FLASH SAVE 1`, reset, `FLASH LOAD 1`, `RUN` works on the dedicated `mmslots` partition.
- 48 KB WiFi-enabled MMBasic heap. ESP32 bytecode compiler scratch tables use ESP-IDF internal heap; VM runtime allocations still use the 48 KB MMBasic heap.
- ESP-IDF detects the onboard PSRAM. The port reserves a contiguous block sized to the detected chip via `heap_caps_aligned_alloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` at boot and publishes it as `PSRAMbase` / `PSRAMsize`; `MM.INFO(PSRAM SIZE)` returns that size and the shared `RAM` command (test / list / save / load / erase) works the same as on Pico variants. `RAM TEST NOCACHE` is Pico-only and errors on ESP32.
- `WEB CONNECT`, `WEB SCAN`, TCP server, TCP client request/stream, UDP send/receive, NTP, and plain-TCP MQTT are hardware-smoked.
- Bundled WEB demos seeded to A: include the small server demo and the multi-file website demo.
- Browser web console over WiFi at `http://<device-ip>/__web_console/` (see [WiFi, Telnet, And Web Console](#wifi-telnet-and-web-console)).
- Telnet console over WiFi with `OPTION TELNET CONSOLE ON`.
- Runtime USB role switching between USB Serial/JTAG console and USB HID host keyboard.
- VGA over LCD_CAM, including 3-bit Serial Wombat wiring, 640 x 480, 320 x 240, and 320 x 240 dithered modes.
- `PLAY TONE` / `PLAY SOUND` / `PLAY NOTE` over external I2S DAC or I2S PDM TX via the shared synthesizer (see [Audio](#audio)).
- `porttools/esp32_fs_vm_smoke.py` default smoke, opt-in flash/VAR persistence, and network conformance have passed on hardware.

Still stubbed or incomplete:

- BASIC-visible GPIO DOUT/DIN/ARAW is hardware-smoked. PWM/servo are still explicit unsupported paths.
- MQTT TLS/cert handling is not implemented; current MQTT support is plain TCP.
- MIDI, ARRAY, and STREAM playback are not wired.
- BLE/Bluetooth and OTA are not implemented.

## Port Tools

Host-side smoke tooling lives in [`../../porttools`](../../porttools/README.md).
Use `basic_serial.py` for prompt-driven command checks and
`esp32_fs_vm_smoke.py` for the Stage G0 device smoke suite. The network suite
chains to `network_conformance.py`; `esp32_tcp_smoke.py` remains available for
narrow TCP client request/stream checks.

Known-good quick checks:

```sh
python3.11 ../../porttools/basic_serial.py \
  --port /dev/cu.usbmodem101 \
  --boot-wait 1 \
  --cmd 'PRINT "ESP32_PROMPT_OK"' \
  --expect ESP32_PROMPT_OK

python3.11 ../../porttools/esp32_tcp_smoke.py \
  --port /dev/cu.usbmodem101 \
  --host 192.168.4.23

python3.11 ../../porttools/esp32_fs_vm_smoke.py psram \
  --port /dev/cu.usbmodem2101 \
  --timeout 12 \
  --long-timeout 120
```

## Build Shape

The ESP32 port owns its runtime/peripheral surface in `main/esp32_*.c` and `main/hal_*_esp32.c`, while reusing the common runtime spine for shared source loading and abort/interrupt helpers where the sequencing matches. It no longer links the host-native runtime or peripheral stubs, and the link intentionally avoids `--wrap` and `--allow-multiple-definition`.

Known remaining cleanup:

- BASIC-visible GPIO uses ESP32-owned `vm_sys_pin_esp32.c` plus the Metro pin table. PWM/servo remains future work.
- Legacy Pico SDK `hardware/*` compatibility headers come from neutral `ports/pico_sdk_compat/`.
- The build defines `MMBASIC_ESP32` only; the temporary `MMBASIC_HOST` compile-mode tag has been removed.
