# ESP32 CYD Port Plan

Status: Phases 1-3 and 8 implemented and smoke-tested on a generic classic
ESP32 devkit (2026-06-11): UART0 REPL with banner/editing/Ctrl-C, LittleFS
`A:` with demo seeding, file `RUN`, classic pin table with
reserved/input-only rejection, and the full Wi-Fi surface — `WEB SCAN`,
`WEB CONNECT` (DHCP), persisted auto-connect at boot, and the
`network_conformance.py all` suite (TCP client/server, UDP, TFTP, telnet
console, NTP, non-TLS MQTT) passing first-attempt on internal RAM only.
Deviation from the Phase 8 baseline text: TLS is compiled in (S3 sdkconfig
parity) rather than stubbed out; whether classic-ESP32 RAM can actually
complete a handshake is untested and belongs to the memory-policy phase. Two shared-port bugs found by the smoke and fixed: the runtime
abort pump's one-slot pushback livelock (any stray byte during RUN made
programs un-interruptible; the pump now drains to `ConsoleRxBuf` like the
Pico UART IRQ), and `FILES` free-bytes using Pico flash-layout math instead
of the mounted lfs geometry. Remaining phases gated on the CYD board.

Plan baseline: re-evaluated 2026-06-11 against `main` @ `a1ad0ed`
— the SDK-compat retirement, SPI-LCD generalization Tracks A and B, the ESP32
runtime-service gate, and the Wi-Fi SPIRAM allocator have all merged. The only
outstanding ESP32 branch is `jv/esp32-dual-console`, an S3-only console change
this port does not depend on (see Phase 2).

Available hardware: a generic classic ESP32 devkit (no PSRAM, no display, no
touch, no SD) covers Phases 1-4 plus LittleFS and Wi-Fi bring-up; a CYD board
(ESP32-2432S028R) expected 2026-06-12 covers the display, touch, and SD
phases.

Target hardware: classic ESP32 "CYD" / ESP32-2432S028R class boards, including
the AITRIP ESP32-2432S028R 2.8 inch 240x320 TFT module. This is not an ESP32-S3
target.

## Goals

Create one configurable `esp32_cyd` ESP-IDF firmware for classic ESP32 boards.
The first useful target is:

- UART REPL over the board's USB-UART bridge.
- Direct-to-panel ILI9341 LCD output with no permanent framebuffer.
- XPT2046 resistive touch.
- Internal flash LittleFS as `A:`.
- Configurable SD card as `B:` where board wiring supports it.
- Wi-Fi and existing ESP32 network command surface where memory allows.
- Board profiles that seed defaults, while keeping all hardware wiring
  overrideable with `OPTION` commands.

Non-goals for the first pass:

- ESP32-S3 support.
- Native USB serial, USB keyboard, or USB role switching.
- ESP32-S3 LCD_CAM VGA.
- A permanent full-screen framebuffer.
- FASTGFX-quality animation.
- Full-size GUI control tables. A small capped GUI-control build is plausible
  after LCD and touch are stable.
- TLS/HTTPS/MQTT-over-TLS in the baseline build.

## Design Direction

Use the existing ESP32-S3 port as the starting point for ESP-IDF integration,
runtime glue, filesystems, Wi-Fi/networking, and generic HAL implementations.
Do not copy the S3 port's display model unchanged: its ILI9341 backend allocates
a 320x240 32-bit shadow buffer in PSRAM, which is not appropriate for no-PSRAM
classic ESP32 CYD boards.

The CYD display path should instead mirror the RAM-constrained RP2040 Wi-Fi
shape: normal drawing commands write directly to the panel. Readback-heavy
features may depend on ILI9341 RAM reads over MISO; if a board cannot support
that reliably, those commands should fail cleanly or use documented degraded
behavior.

SPI-LCD Track B changed how much of the display path is new work. Controller
knowledge — init sequences, `display_details[]`, address windows,
orientation/MADCTL — now lives in the shared `drivers/spi_lcd/spi_lcd_panels.c`
behind the `hal/hal_spi_lcd_bus.h` claim/release transport contract, and the
ESP32 port already implements that contract with `spi_master` transactions in
`esp32_ili9341_lcd.c`. The CYD port reuses both; the only new display code is a
direct-draw presentation layer to replace the S3 port's PSRAM shadow-buffer
presentation. Readback policy maps onto `hal_spi_lcd_bus_read`, which is
capability-gated (the current ESP32 adapter returns 0 = unsupported).

## Phase 1: Skeleton Port

Create `ports/esp32_cyd` as a self-contained ESP-IDF project targeting `esp32`.

Initial source reuse candidates from `ports/esp32_s3/main`:

- MMBasic core source list structure.
- Runtime console glue except for the physical console driver.
- LittleFS and flash storage.
- ESP-IDF Wi-Fi/network HAL and command adapters.
- GPIO, PWM, I2C, time, random, storage stubs, and runtime hooks after audit.
- SD SPI backend after board-profile work.

Remove or stub for classic ESP32:

- USB Serial/JTAG console.
- USB keyboard and USB role option handling.
- ESP32-S3 LCD_CAM VGA.
- Freenove FT6336 touch.
- Freenove ES8311 audio unless a later classic ESP32 board needs it.

Structure (as implemented): `ports/esp32_cyd/main/CMakeLists.txt` includes the
shared source lists from `ports/esp32_s3/main/sources_lists.cmake` (split out
of `sources.cmake` so sibling ports can reuse them), removes the S3-only
TUs, and appends CYD-local replacements. The chip deltas that surfaced:

- `esp32_console.c` (USB Serial/JTAG) → `esp32_cyd_console.c` (UART0).
- USB role/keyboard, VGA scanout → `esp32_cyd_usb_stub.c` /
  `esp32_cyd_vga_stub.c` (which also owns `setmode`/`cmd_mode`).
- S3 pin table → `esp32_cyd_pin_tables.c` (classic 0..39 map).
- The register-level I²C slave ISR uses S3-only register names, so it moved
  to `hal_i2c_esp32_slave.c` behind `hal_i2c_esp32_internal.h`; CYD links an
  ENOSYS stub (`esp32_cyd_i2c_slave_stub.c`). The master half is shared.
- The PDM DAC-style I2S slot config only exists on I2S hardware v2, so the
  slot shape is a per-chip TU (`esp32_audio_pdm_slot_s3.c` /
  `esp32_cyd_audio_pdm_slot.c`).
- `MM.SUPPLY` indexed `ExtCurrentConfig[44]`, past the classic pin table;
  it now reads `HAL_PORT_SUPPLY_ADC_PIN` (44 on every existing port, 0 =
  NULL row on CYD).
- Classic DRAM is the binding constraint at link time: `flash_prog_buf`
  moved from .bss to a boot-time internal-heap allocation (also on S3), and
  the CYD port config trims heap to 32 KB, MAX_VARS to 256, GUI controls
  to 16.

Acceptance:

- `idf.py set-target esp32`
- `idf.py build`
- no dependency on S3-only IDF headers or components.

## Phase 2: UART Console

Replace the S3 native USB console with a UART console suitable for CYD boards'
USB-UART bridge.

The CYD console is a new UART-only source file: the S3 `esp32_console.c`
includes USB Serial/JTAG headers that do not exist for the classic `esp32`
target, so it cannot be shared. The UART head in the unmerged
`jv/esp32-dual-console` branch is a working reference for the
`uart_driver_install` setup, but that branch solves an S3 problem and does
not need to land for this port.

Implement:

- `esp32_cyd_console.c`: UART0 console exposing the same symbol surface the
  shared console glue consumes (`esp32_console_init`,
  `esp32_console_write_bytes`, byte-level reads, pushback)
- raw byte write path used by `MMPrintString` and `MMputchar`
- nonblocking byte read path used by `MMInkey`
- blocking read with timeout for editor input
- ANSI escape decoder integration through the existing console glue
- `hal/hal_serial_console.h` (the raw polled path used by XMODEM): a real
  implementation over UART0, or the stub whose `enter()` returns false until
  XMODEM is in scope
- no `OPTION USB` surface on classic ESP32

Acceptance:

- boot reaches the MMBasic banner and prompt over UART
- `PRINT MM.VER`
- line editing
- `RUN`, `LIST`, `NEW`
- Ctrl-C interruption

## Phase 3: Classic ESP32 Port Configuration

Add a classic ESP32 `port_config.h`, sdkconfig defaults, and pin table.

Important differences from ESP32-S3:

- GPIO range is 0..39, not 0..48.
- GPIO 34..39 are input-only.
- flash pins must be reserved.
- strapping pins need conservative defaults.
- ADC2 conflicts with Wi-Fi, so analog support must be conservative; the
  ADC policy lives behind the `hal/hal_adc.h` contract.
- no native USB pins.
- no S3-only high GPIOs.
- cycle counter: classic ESP32 has the same Xtensa CCOUNT as the S3, so
  `ports/esp32_s3/main/hal_cycle_counter_inlines.h` should carry over.

Pin table requirements:

- `GPn` syntax maps to valid classic ESP32 GPIOs.
- unavailable pins are marked `UNUSED`.
- input-only pins do not advertise output or PWM.
- PWM mappings match the LEDC backend actually available on ESP32.
- CYD profile-reserved pins are reserved by options/profile, not hard-coded as
  globally unavailable.

Acceptance:

- `SETPIN`
- `PIN()`
- digital input/output smoke
- PWM/servo smoke where supported
- invalid/reserved pins rejected cleanly

## Phase 4: Board Profiles

The profile mechanism already exists: `esp32_board_profile.c` implements
`CONFIGURE LIST`/`CONFIGURE <name>`, `MM.INFO$(DEVICE)`, profile-seeded option
defaults, and safe-boot to `GENERIC` on a bad saved configuration. This phase
extends the profile table for classic ESP32 rather than building the
mechanism: add the CYD entry, and audit the `GENERIC` defaults (which assume
S3 pin numbering) for the classic ESP32 GPIO map.

Profiles:

- `GENERIC`: UART, LittleFS, Wi-Fi-capable, no display/touch/SD pins claimed.
- `CYD ILI9341`: common ESP32-2432S028R defaults.

Required command behavior:

- `CONFIGURE LIST`
- `CONFIGURE GENERIC`
- `CONFIGURE CYD`
- `MM.INFO$(DEVICE)` identifies the active profile.
- `LIST OPTIONS` prints active display, touch, SD, and relevant system wiring.

Profiles should apply defaults, save options, and reboot. A bad saved
peripheral configuration should safe-boot to `GENERIC` without making the board
unrecoverable.

Acceptance:

- clean flash boots `GENERIC`
- `CONFIGURE CYD` seeds display/touch/SD defaults
- `CONFIGURE GENERIC` clears board peripheral claims
- manual `OPTION` overrides survive reboot

## Phase 5: Direct ILI9341 LCD

Build a direct-to-panel presentation layer on the pieces that already exist:
`drivers/spi_lcd/spi_lcd_panels.c` (controller init/window/orientation) driven
through the ESP32 `hal_spi_lcd_bus` adapter (`spi_master` transactions). The
new work is replacing the S3 presentation model — `esp32_ili9341_lcd.c`
allocates its 320x240 32-bit shadow with `MALLOC_CAP_SPIRAM`, which fails
outright on a no-PSRAM board — with draw hooks that write panel windows
directly, with no permanent full-screen buffer.

Option surface (already implemented by Track A in `esp32_lcd_options.c`):

- `OPTION SYSTEM SPI clk, mosi, miso`
- `OPTION LCDPANEL ILI9341, orientation, dc, reset, cs [, bl] [, INVERT]`
- disable forms matching the existing ESP32 option style

Gaps to close in the existing surface:

- orientation currently accepts only `LANDSCAPE`/`L`; the CYD panel is
  240x320 portrait-native, so the full orientation set must parse and map to
  MADCTL through the shared panel core.
- backlight control should land behind `hal/hal_display_backlight.h` (the S3
  port's `esp32_backlight.c` is the starting point).

Required drawing support:

- `CLS`
- console text
- `PIXEL`
- `LINE`
- `BOX`
- `CIRCLE`
- `TEXT`
- `IMAGE`/bitmap writes where existing shared code can stream through
  `DrawBuffer`
- backlight PWM

Readback policy:

- If ILI9341 readback over MISO works, implement `hal_spi_lcd_bus_read` (the
  current ESP32 adapter returns unsupported) and wire `ReadBuffer` through it.
- If readback is unreliable or absent, leave readback unsupported and make
  readback-dependent commands fail clearly.
- Document any degraded behavior for sprites, alpha/AA drawing, `PIXEL(x,y)`,
  transparent blits, and scroll modes.

Acceptance:

- visible MMBasic console on the LCD
- basic graphics demos run without PSRAM
- no permanent allocation on the order of 320x240x4 bytes
- display remains usable with Wi-Fi enabled

## Phase 6: XPT2046 Touch

Add a resistive touch driver for CYD-class XPT2046 wiring.

Required option surface:

- `OPTION TOUCH XPT2046, cs [, irq]`
- calibration storage compatible with the existing touch option style where
  practical
- disable form

Implementation notes:

- A shared XPT2046-class resistive touch driver already exists:
  `drivers/gui_touch/Touch.c` (with pin glue in
  `drivers/spi_lcd/spi_lcd_periph_io.c`) implements `OPTION TOUCH` with
  CS/IRQ pins on PicoMite. Evaluate reusing it over the ESP32 SPI transport
  before writing a new driver; the catch is that it consumes the Pico bus
  arbiter symbols (`SetCS`/`lcd_xmit_byte_multi`), which the
  `hal_spi_lcd_bus` contract does not yet cover for touch transactions.
- The existing ESP32 `OPTION TOUCH` form is FT6336/I2C only; the XPT2046
  SPI-CS form is new option surface.
- Touch may share the display SPI bus but must have independent CS handling.
- Support common CYD rotation/orientation mapping.
- Do not hard-code CYD pins outside the board profile defaults.

Acceptance:

- touch raw read smoke
- calibration command works
- calibrated coordinates track the displayed orientation

## Phase 7: Filesystems

Bring up storage in order:

1. Internal flash LittleFS as `A:`.
2. Configurable SD card over SPI as `B:`.

SD should be added after LCD and touch because CYD clones vary and may share SPI
buses or pins differently.

Acceptance:

- `FILES`
- `SAVE`
- `LOAD`
- `OPEN`
- `PRINT #`
- `INPUT #`
- `KILL`
- `COPY`
- SD mount/read/write if the board wiring supports it

## Phase 8: Wi-Fi And Network

Reuse the ESP32-S3 ESP-IDF network HAL and command adapters where compatible.
Classic ESP32 memory pressure is higher, so bring networking up after UART,
LittleFS, LCD, and touch are stable.

The baseline CYD build should omit TLS support. Plain Wi-Fi, TCP, UDP, Telnet,
HTTP, and non-TLS MQTT are the first target. TLS pulls in mbedTLS runtime state,
certificate handling, and larger transient connection buffers; it should remain
a later opt-in feature only if measured internal-RAM headroom supports it.

Acceptance:

- `OPTION WIFI ssid, password`
- `MM.INFO$(IP ADDRESS)`
- TCP client smoke
- telnet/web console only if memory testing is acceptable
- TLS commands report "TLS not supported on this port" in the baseline build
- boot does not initialize Wi-Fi unless configured or explicitly requested

## Phase 9: Memory Policy

Default first-pass policy:

- no permanent framebuffer
- no S3 USB stack
- no VGA
- no TLS
- defer GUI controls until LCD/touch are stable, then test with a small cap
  such as 16 controls
- bytecode VM disabled initially if RAM is tight
- conservative MMBasic heap size
- conservative Wi-Fi buffers
- TLS/MQTT tested separately after baseline stability

Potential later improvements:

- optional small packed display buffer, such as RGB121 or RGB332
- optional PSRAM-aware mode for CYD variants that actually include PSRAM
- larger GUI-control caps if memory measurements justify it

Acceptance:

- stable boot on no-PSRAM board
- REPL, LCD, touch, LittleFS, and Wi-Fi can coexist
- memory failure paths produce errors, not boot loops

## Phase 10: Smokes, Build Helper, And Docs

Add:

- `buildesp32.sh esp32_cyd` support
- `ports/esp32_cyd/README.md`
- flashing instructions for ESP32-2432S028R/AITRIP boards
- known pinout table with clone caveats
- UART REPL smoke
- LittleFS smoke
- LCD graphics smoke
- touch calibration smoke
- Wi-Fi smoke
- optional SD smoke

Document:

- no-framebuffer display behavior
- readback limitations
- which commands degrade or error without LCD readback
- how to recover with `CONFIGURE GENERIC`
- how to manually configure alternate clone pinouts

## Bring-Up Sequencing

Two hardware stages:

1. **Generic classic ESP32 devkit (in hand)** — no PSRAM, display, touch, or
   SD. Validates Phases 1-4 (skeleton build, UART console, pin tables,
   `GENERIC` profile), internal-flash LittleFS from Phase 7, and the Phase 8
   Wi-Fi/network baseline. Memory measurements for the Phase 9 policy start
   here: this board is the worst case the port must boot on.
2. **CYD ESP32-2432S028R (expected 2026-06-12)** — adds Phases 5-6 (ILI9341,
   XPT2046), the `CYD` profile, and SD.

Working the storage and network phases ahead of the display phases inverts
the written phase order; that is fine — the phases were ordered by
dependency, and LittleFS/Wi-Fi depend only on the Phase 1-3 base.

## VGA On Generic Classic-ESP32 Boards (in progress)

Implemented and hardware-validated 2026-06-11 on a generic devkit + Gert
VGA666 module (RGB222 wiring: two MSB ladder taps per channel, syncs on
header pins 5/3):

- `drivers/vga_i2s_esp32/` — the I2S1 LCD-mode scanout core: APLL
  25.175 MHz pixel clock, 525-descriptor loop (one 800-byte buffer per
  line, two shared blanking buffers, an 8-buffer visible-line ring), and a
  per-line EOF ISR driving a pluggable line-fill callback. Sync rides bits
  6/7 of every byte; the FIFO emits word bytes in 2,3,0,1 order
  (`VGA_I2S_PX` / `VGA_I2S_PACK4` encapsulate the swizzle). ~14 KB DMA RAM.
  The fill interrupt MUST live on core 1 at level 3 (a pinned helper task
  performs the alloc): core 0 carries MMBasic + Wi-Fi + flash-write
  critical sections whose interrupt-blocked windows exceed the ring slack
  and scan out as horizontal glitch bands — observed on hardware, cured
  by the core move. With that, the text console is visually solid with
  Wi-Fi associated and under traffic.
- `ports/esp32_cyd/main/esp32_cyd_vga.c` — `OPTION VGA` (char-cell
  console), `OPTION VGA TEST [RED|GREEN|BLUE|WHITE|LADDER]` (test card +
  per-wire ladder diagnostic, which found two miswired ladder taps during
  bring-up), `OPTION VGA DISABLE`.
- `ports/esp32_cyd/main/esp32_cyd_vga_text.c` — the MODE 3 char-cell
  console: 80x40 cells, 8x12 MMBasic font copied to RAM (the ISR cannot
  read flash during LittleFS writes), per-cell RGB222 fg/bg, VT100 subset
  (SGR 24-bit colours, cursor motion, clears), hooked into `putConsole`
  beside the serial sink.

Screen modes (implemented and hardware-validated 2026-06-11):

- `MODE 1` — 640x480 1bpp graphics (`esp32_cyd_vga_mode1.c`): bit-packed
  framebuffer (38.4 KB on demand), full draw-hook set with the port-wide
  3-bytes-per-pixel RGB888 buffer contract, fg/bg latched from the console
  colours, mask-LUT expansion in the fill ISR.
- `MODE 2` — 320x240 **16-colour** graphics (revised from the RGB222
  64-colour sketch): the 4bpp RGB121 framebuffer (38.4 KB) drives the
  shared Draw.c `*16` primitive family unchanged; the fill ISR maps
  nibbles through the shared `colours[]` palette to RGB222 with
  pixel/line doubling. 16 colours matches VGA PicoMite MODE 2, and 4bpp
  fits classic-ESP32 DRAM (the 77 KB byte-per-pixel buffer did not: the
  largest free block measured 73.7 KB).
- `MODE 3` — the char-cell console; graphics-mode exits and allocation
  failures always land here, never a dead screen.
- `DISPLAY_TYPE`/`Option.DISPLAY_TYPE` carry SCREENMODE1/2 during
  graphics (runtime only, never saved): the graphics commands gate on
  `Option.DISPLAY_TYPE` and do_end's `DISPLAY_TYPE - SCREENMODE1 + 1`
  round-trips the mode number. Mode switches free the outgoing
  framebuffer before allocating the next (two cannot coexist).
- Wi-Fi mutual exclusion: boot-with-Wi-Fi lands in MODE 3; `MODE 1/2`
  error while `WIFIconnected`; verified both directions on hardware.
- The glass-terminal hook moved from `putConsole` to `SerialConsolePutC`
  so the MODE 3 console mirrors the full serial byte stream (including
  SSPrintString-only VT100 escapes).

Also implemented (2026-06-11, second pass): boot persistence — the pin
map lives in the Option.extensions[] VGA region and
`esp32_vga_display_init` brings the MODE 3 console up automatically at
boot; the pin-list form `OPTION VGA r1,r0,g1,g0,b1,b0,hsync,vsync` (chip
GPIOs, validated output-capable, `OPTION LIST` round-trips it); and full
16-colour ANSI SGR in the glass terminal, which is what the editor's
COLOURCODE highlighting and `COLOUR` both need (verified green on
glass).

Filesystem performance (measured 2026-06-11): LittleFS directory-read
cost grows with write churn (~10 ms/entry fresh -> 40+ ms after heavy
writes; the metadata log is walked per read until compaction), and FILES
re-walks the whole directory once per printed line (deliberate
O(1)-memory selection sort), multiplying that into tens of seconds. The
per-boot demo rewrites were the main churn driver (now version-stamped
away). Next: a read-once flist[] in cmd_files bounded by
HAL_PORT_FILES_MAX makes listing cost churn-independent; also evaluate
lfs_fs_gc for proactive compaction, and note the VGA scanout ISR adds
~4x to raw flash-op cost (secondary, plan-noted).

Still to do: `OPTION WIFI "",""` should actively disconnect instead of
needing a reboot before the graphics modes unlock, a VGA board profile,
wider console fonts (any width divisible by 4: 12x20, 16x24), scroll
tearing (replace the scroll memmove with a row-remap table),
Wi-Fi-radio-on-but-unconnected slipping past the `WIFIconnected` gate,
and the line-editor redraw chatter visible in serial transcripts
(cosmetic, predates VGA).

### Design notes (original sketch)

Classic ESP32 has no LCD_CAM, so the S3 VGA driver cannot be reused — but
the chip's I2S1 parallel "LCD mode" streams bytes to pins via DMA with zero
CPU on the pixel clock, and FabGL / bitluni / the TTGO VGA32 boards prove
VGA output on exactly this silicon. Bit-banged VGA is explicitly rejected
(tried previously; sync timing fights Wi-Fi/cache and consumes a core).

Hardware shape: 8 output GPIOs (6 color + HSync + VSync — on classic ESP32
the sync levels must ride inside every streamed byte, so color is RGB222 /
64 colors, not the S3's RGB332) plus a resistor DAC. Viable on bare devkits
and VGA32-class boards as a board profile; not wireable on CYD hardware,
whose spare connectors expose only ~2 output-capable pins. Port the
FabGL/bitluni descriptor technique rather than writing a new scanout.

Canonical three-mode design (settled 2026-06-11):

- **MODE 1 — 640x480 1bpp graphics** (matches VGA PicoMite MODE 1).
  Packed monochrome framebuffer (~38 KB) allocated on demand, with a
  per-scanline 1bpp -> byte-stream expansion ISR on core 1 (idle in this
  build; Wi-Fi pins to core 0), LUT-driven. Full graphics commands; text
  via the normal pixel console with the existing MMBasic fonts.
- **MODE 2 — 320x240 RGB222 graphics** (matches VGA PicoMite MODE 2 in
  shape; 64 colors here). One byte per pixel (~77 KB) allocated on demand;
  the DMA descriptor chain streams it directly (zero scanout CPU) with
  pixel-doubling in the line and descriptor-level line repeat doing the
  320x240 -> 640x480 upscale for free. Reuses the one-byte-per-pixel draw
  layer with an RGB222 encode macro. Interpreter-only: the ~20 KB of
  remaining slack does not fit FRUN compiler scratch.
- **MODE 3 — 640x480 char-cell text, the safety floor.** Character +
  attribute buffer (~6.5 KB at 8x12) rendered by a glyph-expansion ISR
  reading the existing MMBasic font tables, per-cell colors from the
  RGB222 palette. Allocated once at VGA bring-up and never freed, so a
  working console always exists while VGA is the active display; MODE 1/2
  allocation failures error into the still-working MODE 3 console.
  Console-only: graphics commands error in MODE 3. FRUN works (~90 KB
  slack). Coexists with Wi-Fi.
- **Wi-Fi policy.** The MODE 1/2 framebuffers and the Wi-Fi stack want the
  same internal RAM, so: booting with Wi-Fi configured lands in MODE 3,
  and MODE 1 / MODE 2 fail with a clear error while Wi-Fi is up
  (symmetrically, enabling Wi-Fi requires MODE 3). Booting without Wi-Fi
  lands in MODE 1, the PicoMite-style default. Errors, never boot loops.
- Boards that never configure `OPTION VGA` pay nothing: every buffer above
  is allocated at or after VGA bring-up, none statically.

## Main Risks

The main risk is not running MMBasic on classic ESP32; the existing ESP-IDF port
already proves most of that path. The real risks are:

- direct ILI9341 rendering compatibility with existing graphics expectations
- LCD readback variability across CYD clones
- total internal RAM pressure with Wi-Fi plus display/touch/SD
- ADC2 limitations when Wi-Fi is active
- clone pinout differences

The first implementation should deliberately keep the feature set narrow and
recoverable. Once a no-framebuffer `GENERIC` and `CYD` build is stable, add
features only when memory measurements support them.
