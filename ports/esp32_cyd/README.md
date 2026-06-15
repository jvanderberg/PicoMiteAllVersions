# MMBasic Anywhere - ESP32 (CYD)

**Download:** prebuilt firmware is published on the [latest release](https://github.com/jvanderberg/PicoMiteAllVersions/releases/tag/latest) as `MMBasic-Anywhere-esp32-cyd-merged.bin`. Flash it to `0x0` in DIO mode; see [Flash](#flash). Building from source is only needed for development.

This is the **classic ESP32** port (the original dual-core ESP32, *not* the S3). It targets the ESP32-2432S028R "Cheap Yellow Display" (CYD / CYD2USB) and runs equally on a plain ESP32 dev board with no display. The classic ESP32 has **no PSRAM**, so MMBasic runs in the chip's internal RAM — about a 48 KB program/variable heap. That budget shapes what the port can do (see [No-PSRAM limits](#no-psram-limits)): no permanent off-screen framebuffer, no TLS, and Wi-Fi or a graphics/`FRUN` workload, but not both at once.

One firmware image serves both the CYD and a bare dev board; a board profile (`CONFIGURE CYD` / `CONFIGURE GENERIC`) selects the wiring. For the PSRAM-equipped ESP32-S3 boards, see [`../esp32_s3/README.md`](../esp32_s3/README.md).

Plan: [docs/real-hal/esp32-cyd-port-plan.md](../../docs/real-hal/esp32-cyd-port-plan.md).

## Prerequisites

- A classic ESP32 board over USB. The CYD exposes a CP2102/CH340 **USB-UART bridge**, not native USB — install that vendor driver if your OS lacks it. The port enumerates as `/dev/cu.usbserial-*` or `/dev/cu.SLAB_USBtoUART*` (macOS/Linux) or `COMx` (Windows, under **Device Manager → Ports**).
- ESP-IDF 5.3 installed at `~/esp/esp-idf/` (source builds only).
- `pyserial` if using `probe.py` / the smoke suites.

For a release-image flash you only need [`esptool`](https://github.com/espressif/esptool) (`pip install esptool`) — no ESP-IDF checkout.

## Build

The port is a self-contained ESP-IDF project. Load the environment, then build the standard way or via the repo-root helper (which runs the HAL purity gate first):

```sh
. ~/esp/esp-idf/export.sh

cd ports/esp32_cyd
idf.py set-target esp32        # one-time, or after idf.py fullclean
idf.py build
```

```sh
./buildesp32.sh esp32_cyd      # from the repo root
./buildesp32.sh all            # esp32_s3 octal + quad + esp32_cyd
```

The firmware is written to `ports/esp32_cyd/build/mmbasic_anywhere_esp32_cyd.bin`.

## Flash

### Flash a release build

Download `MMBasic-Anywhere-esp32-cyd-merged.bin` from the [latest release](https://github.com/jvanderberg/PicoMiteAllVersions/releases/tag/latest) and write it with `esptool`. Replace the port with your device's node. Erase first, then write the merged image at `0x0` — a bare reflash leaves the A: filesystem and `FLASH SAVE` slots untouched, so erasing guarantees a clean, deterministic state:

```sh
esptool --chip esp32 -p /dev/cu.usbserial-0001 erase_flash
esptool --chip esp32 -p /dev/cu.usbserial-0001 write_flash 0x0 MMBasic-Anywhere-esp32-cyd-merged.bin
```

The merged image already carries the classic-ESP32 layout — bootloader at `0x1000`, partition table at `0x8000`, app at `0x10000` — and a DIO header, so the command above is correct. Most CYDs auto-enter download mode (esptool pulses DTR/RTS). If the connect hangs on `Connecting...`, hold **BOOT (IO0)**, tap **EN/RST**, release BOOT, and re-run.

A clean flash boots the `GENERIC` profile. On a CYD, run `CONFIGURE CYD` once to wire the display, touch, and SD card; see [Board Profiles](#board-profiles).

### Does skipping the erase matter?

It can. The merged image overwrites only bootloader, partition table, NVS, and the app. The two data partitions above the app — `lfsdata` (the A: LittleFS drive) and `mmslots` (`FLASH SAVE` / `VAR SAVE`) — are **not** touched. Reflashing the same firmware lineage without erasing is fine and keeps your A: files and saved slots. The hazard is partition-table drift: moving from a build with a different partition map (or from Arduino/MicroPython) leaves A: and the slots pointing at the old layout, so LittleFS may fail to mount or `FLASH LOAD` reads garbage. When in doubt — version changes, switching firmware, or anything acting weird — erase first.

### Flash from source

With ESP-IDF loaded and the port built:

```sh
cd ports/esp32_cyd
idf.py -p /dev/cu.usbserial-0001 flash
```

## Monitor And Probe

The console is a full MMBasic REPL over UART0 (TX GPIO1 / RX GPIO3) through the onboard USB-UART bridge, 115200 baud, 8N1. Use the `/dev/cu.*` node on macOS:

```sh
screen /dev/cu.usbserial-0001 115200      # exit: Ctrl-A then K, then y
```

`minicom`, `tio`, and the ESP-IDF monitor (`idf.py -p PORT monitor`, exit Ctrl-]) also work. Only one program can hold the port at a time — close the terminal before running the smoke suites, and vice versa. For scripted one-shot commands:

```sh
python3 probe.py /dev/cu.usbserial-0001 --cmd 'PRINT MM.VER'
```

## Board Profiles

One firmware image serves both the CYD and a bare dev board. A board profile carries the default wiring for that board's LCD, SD card, touch, and audio; the active profile is saved and reapplied on every boot. Select one with the `CONFIGURE` command (an alias for `OPTION RESET`) — it resets options to that board's defaults, saves, and reboots:

```basic
CONFIGURE LIST       ' GENERIC / CYD / METRO / FREENOVE ILI9341
CONFIGURE CYD        ' apply CYD defaults (ST7789 + XPT2046 + SD) and restart
CONFIGURE GENERIC    ' conservative defaults, no board peripherals
```

`MM.INFO$(DEVICE)` reports the active board (`MMBasic ESP32 CYD` after `CONFIGURE CYD`). A clean flash, and the safe-boot fallback after a crash, both land on `GENERIC` — the saved profile on flash is preserved either way.

| Profile | Hardware | What the profile wires up |
|---|---|---|
| `GENERIC` | Any classic ESP32 dev board | UART0 REPL, `A:` (LittleFS), Wi-Fi. No display, SD, touch, or audio assumed. |
| `CYD` | ESP32-2432S028R / CYD2USB | ST7789 LCD, XPT2046 touch, microSD, internal-DAC audio. |

The `METRO` and `FREENOVE ILI9341` profiles also appear in `CONFIGURE LIST` (the table is shared with the ESP32-S3 build) but describe S3 boards — they are not for classic-ESP32 hardware.

### CYD board

`CONFIGURE CYD` seeds the ESP32-2432S028R / CYD2USB wiring. The display is a **240×320 ST7789** panel; the board has **no reset line** (RST is tied to the board), so the profile drives it accordingly with non-inverted (BGR) polarity. Defaults:

| Peripheral | Wiring (GPIO) |
|---|---|
| LCD (ST7789, SPI3 / HSPI, 40 MHz) | SCLK 14, MOSI 13, MISO 12, CS 15, DC 2, RST none, BL 21 |
| Touch (XPT2046, separate SPI) | SCLK 25, MOSI 32, MISO 39, CS 33, IRQ 36 |
| microSD (SPI) | SCLK 18, MOSI 23, MISO 19, CS 5 |
| Audio | internal DAC on GPIO 26 |

The LCD draws **direct to the panel** — there is no off-screen framebuffer (see [No-PSRAM limits](#no-psram-limits)). Graphics (`CLS`, `PIXEL`, `LINE`, `BOX`, `CIRCLE`, `TEXT`, `IMAGE`), the text console, and GUI controls all render straight to glass; `REFRESH` / `AUTOREFRESH` are not applicable and report `Invalid on this display`. Calibrate the resistive touchscreen with `GUI CALIBRATE`. The display, touch, SD, and audio are all `OPTION`-driven, so you can re-wire any of them by hand exactly as on the other PicoMite ports — the profile only supplies defaults.

### Generic ESP32 dev board (no display)

On a plain ESP32 dev board with no panel, stay on `GENERIC`. It brings up only what every classic ESP32 has — the UART0 REPL over the USB-UART bridge, the `A:` LittleFS filesystem, and Wi-Fi/networking — and reserves **no** GPIO for a display, SD bus, or audio, so it runs on any board without pin conflicts. Everything that does not need a screen works: `PRINT`, control flow, `EDIT`, `LIST`, the file commands, `FLASH SAVE`/`LOAD`, and the full networking surface below. To attach peripherals later, configure them by hand:

```basic
OPTION SYSTEM SPI GP14,GP13,GP12         ' LCD SPI bus: clk, mosi, miso
OPTION LCDPANEL ST7789, LANDSCAPE, GP2, 0, GP15, GP21
                                         ' controller, orientation, DC, RST (0 = none), CS [, BL]
OPTION TOUCH XPT2046, GP33, GP36         ' cs, irq
OPTION SDCARD GP5,GP18,GP23,GP19         ' cs, clk, mosi, miso
CPU RESTART
```

## Wi-Fi And Network

Wi-Fi credentials are stored in NVS. `OPTION WIFI` connects immediately and reuses the saved credentials on later boots:

```basic
OPTION WIFI "your-ssid","your-password"
PRINT MM.INFO$(IP ADDRESS)
```

Plain TCP and UDP, the TCP server, telnet console (`OPTION TELNET CONSOLE ON`), the browser web console (`OPTION WEB CONSOLE ON`, served at `http://<device-ip>/__web_console/`), NTP, and plain-TCP MQTT are all supported and hardware-smoked. **TLS/HTTPS and MQTT-over-TLS are not available** — the mbedTLS handshake buffers do not fit in internal RAM without PSRAM.

Because Wi-Fi consumes most of the internal heap, the device boots into the text console (`MODE 3`) whenever Wi-Fi is configured, and graphics modes report a clear error while Wi-Fi is connected. See [No-PSRAM limits](#no-psram-limits).

## VGA

The CYD port can drive a resistor-DAC ("VGA666") VGA monitor instead of the onboard LCD. `OPTION VGA` reuses the LCD/SD GPIOs, so VGA and the onboard ST7789 panel are mutually exclusive — VGA is an alternative output, not a second screen.

```basic
OPTION VGA                               ' enable on the default pin map
OPTION VGA r1,r0,g1,g0,b1,b0,hs,vs       ' enable on explicit chip GPIOs
OPTION VGA TEST LADDER                   ' per-wire diagnostic (RED|GREEN|BLUE|WHITE|LADDER)
OPTION VGA DISABLE                       ' stop, release pins, clear saved config
```

The default pin map uses GPIOs 4, 5, 18, 19, 21, 22, 23, 15. Screen modes, while `OPTION VGA` is active:

| Command | Framebuffer | Notes |
|---|---|---|
| `MODE 1` | 640×480 1bpp | requires Wi-Fi off |
| `MODE 2` | 320×240 16-colour | requires Wi-Fi off; matches the VGA PicoMite `MODE 2` |
| `MODE 3` | char-cell text console | the `OPTION VGA` default; coexists with Wi-Fi |

`MODE 1`/`MODE 2` allocate a framebuffer from the internal heap, so they need Wi-Fi disabled (`OPTION WIFI` cleared, or boot without credentials).

## Audio

`PLAY TONE`, `PLAY SOUND`, and `PLAY NOTE` are synthesized by the shared software synthesizer (`shared/audio/synth_pcm.c`) and output to the CYD's onboard amplifier through the **internal DAC on GPIO 26** (the `CONFIGURE CYD` default). File playback (`PLAY WAV` / `FLAC` / `MP3` / `MODFILE`) runs through the shared decoder path. MIDI, ARRAY, and STREAM playback are not wired in this port.

## No-PSRAM limits

The classic ESP32 has no PSRAM, so MMBasic lives in ~48 KB of internal RAM. The consequences are deliberate, not bugs:

- **No permanent framebuffer.** The LCD draws direct to glass; `REFRESH`/`AUTOREFRESH` and `FRAMEBUFFER` are rejected (`FRAMEBUFFER not supported on this display`).
- **No TLS.** HTTPS and MQTT-over-TLS need handshake buffers that do not fit in internal RAM.
- **Wi-Fi *or* a heavy graphics/`FRUN` workload, not both.** The Wi-Fi stack claims most of the heap, so with Wi-Fi up the board runs the text console (`MODE 3`); the VGA graphics modes and large `FRUN` programs need Wi-Fi off.

For headroom — full graphics with a framebuffer, TLS networking, and `FRUN` together — use a PSRAM-equipped ESP32-S3 board ([`../esp32_s3/README.md`](../esp32_s3/README.md)).

## Port Tools

Host-side smoke tooling lives in [`../../porttools`](../../porttools/README.md). The CYD display path has a dedicated smoke:

```sh
python3 ../../porttools/esp32_cyd_display_smoke.py --port /dev/cu.usbserial-0001
```

`basic_serial.py` drives prompt-based command checks, `esp32_fs_vm_smoke.py` covers the filesystem/VM surface, and `network_conformance.py` exercises the (non-TLS) network stack. Close any open terminal before running them.
