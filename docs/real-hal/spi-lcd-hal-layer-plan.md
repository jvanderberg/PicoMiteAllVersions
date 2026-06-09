# SPI LCD Generalization Plan

**Status:** planned, not started. Track A is independent and lands first;
Track B is the structural refactor and follows.

Two tracks, separable and sequenced:

- **Track A — user-configurable display pins on ESP32-S3.** Port-local,
  small, fixes the reported problem: the Freenove ILI9341 profile hard-codes
  GPIO assignments, so the same panel wired to a generic ESP32-S3 board on
  different pins is unusable. After Track A, `OPTION LCDPANEL` assigns pins
  at runtime on ESP32 exactly as it does on every PicoMite.
- **Track B — transport-neutral SPI LCD controller core.** Structural: split
  `drivers/spi_lcd/spi_lcd.c` into portable controller knowledge (28 panel
  init sequences, address windows, orientation) and a per-port bus transport,
  so the ESP32 port inherits the full controller family instead of
  maintaining a parallel one-controller driver.

Track A's `Option.LCD_*` plumbing is exactly the pin source Track B's shared
driver reads, so nothing in Track A is throwaway.

## Current state (verified against the tree)

### PicoMite: fully user-configurable, pico-sdk-bound

- `OPTION LCDPANEL <ctrl>, <orient>, CD, RST, CS [,BL]` →
  `ConfigDisplaySPI()` (`drivers/spi_lcd/spi_lcd.c:176`) →
  `Option.DISPLAY_TYPE / LCD_CD / LCD_Reset / LCD_CS / DISPLAY_BL` →
  `SaveOptions()` → reboot → `InitDisplaySPI()` (`spi_lcd.c:300`) reads the
  Option fields. Bus pins come from `OPTION SYSTEM SPI`
  (`Option.SYSTEM_CLK/MOSI/MISO`); rp2350 NEXTGEN supports a dedicated LCD
  bus (`Option.LCD_CLK/MOSI/MISO`, `core/mmbasic/FileIO.h:158-160`).
- 28 controllers in one driver: `display_details[]` (`spi_lcd.c:34-103`)
  plus per-controller init sequences in `InitDisplaySPI`.
- The file only compiles against pico-sdk: `hardware/dma.h`,
  `hardware/gpio.h`, `pico/multicore.h`, `spi_inst_t`, PL022 FIFO registers,
  `__not_in_flash_func`.

### ESP32-S3: profile-fixed pins, single controller

- Pins live in the static profile table (`esp32_board_profile.c:23-107`);
  FREENOVE_ILI9341 hard-codes SCLK=12 MOSI=11 MISO=13 CS=10 DC=46 BL=45.
- `esp32_ili9341_lcd_init()` (`esp32_ili9341_lcd.c:748`) reads
  `profile->lcd.*` directly. The Option fields are seeded by
  `esp32_board_profile_apply_defaults()` (`esp32_board_profile.c:242-255`)
  but never read back — they are dead state on this port.
- The only user control is `OPTION RESET FREENOVE ILI9341` (whole-profile
  selection). No per-pin assignment exists.
- The port already proves the runtime-pin pattern elsewhere:
  `OPTION SDCARD cs, clk, mosi, miso` (`esp32_peripheral_stubs.c:371`).

### The transport choke points in `spi_lcd.c`

All 28 controller init sequences and the OPTION parsing funnel through five
primitives; the pico-sdk surface is much smaller than the file:

| Primitive | Today | Portability |
|---|---|---|
| `lcd_xmit_byte_multi(buf, n)` | already a function pointer | port assigns it |
| `gpio_put(LCD_CD_PIN, hi/lo)` | direct pico-sdk, 29 call sites | wrap |
| `SetCS()` / `ClearCS()` | helpers | wrap |
| `PinSetBit(Option.LCD_Reset, …)` | MMBasic pin layer | already abstract |
| `uSec(n)` | MMBasic timing | already abstract |

Genuinely pico-bound and staying per-port: bus bring-up
(`spi_init`/`spi_set_format`/`gpio_set_function`, `spi_lcd.c:322-331`), the
PL022 fast-write path (`spi_write_fast`/`spi_finish`, `spi_lcd.c:104-125`),
DMA, and the core1 merge pipeline (already isolated in
`drivers/display_merge/` with a stub pair).

---

## Track A — user-configurable display pins on ESP32-S3

Goal: `Option.LCD_*` becomes the single source of truth for display pins on
the ESP32 port; the board profile demotes to a defaults seeder; users on any
wiring run `OPTION LCDPANEL` once. No shared-file changes; the host suite is
untouched by construction.

### A1 — drivers read Option, not the profile

- `esp32_ili9341_lcd.c`: replace every `profile->lcd.*` read in
  `esp32_ili9341_lcd_init()` (and the restore path
  `esp32_ili9341_lcd_restore_panel()`) with the corresponding Option field:
  bus pins `Option.LCD_CLK/LCD_MOSI/LCD_MISO`, control pins
  `Option.LCD_CS/LCD_CD/LCD_Reset`, backlight `Option.DISPLAY_BL`. Gate on
  `Option.DISPLAY_TYPE == ILI9341` plus pin validity instead of
  `profile->has_lcd`.
- `esp32_backlight.c`: `backlight_pin()` reads `Option.DISPLAY_BL`.
- Extend `esp32_board_profile_apply_defaults()` to seed `Option.DISPLAY_BL`
  (the other LCD fields are already seeded).
- Pin sentinel: Option pin fields use 0 = unconfigured (the cross-port
  convention). GPIO0 is an ESP32 strapping pin and is not assignable to the
  display; the setter rejects it, matching the SDCARD setter's posture.
- SPI clock: keep the 40 MHz default currently in the profile as the
  driver-level default for ILI9341. A per-panel speed table arrives with
  Track B; no new Option field now.

Exit: Freenove board behaves identically (profile seeds the same pins);
`grep -n "profile->lcd" ports/esp32_s3/main/*.c` returns only the
defaults-seeding site.

### A2 — `OPTION LCDPANEL` setter on the ESP32 port

- New file `ports/esp32_s3/main/esp32_lcd_options.c` (the peripheral-stubs
  monolith is slated for breakup; don't grow it). Wire
  `esp32_lcdpanel_option_setter(cmdline)` into the port's `cmd_option`
  chain (`esp32_peripheral_stubs.c:411`) alongside the SDCARD setter.
- Syntax mirrors PicoMite so documentation and muscle memory transfer:

  ```
  OPTION SYSTEM SPI clk, mosi, miso          ' bus pins → Option.LCD_CLK/MOSI/MISO
  OPTION LCDPANEL ILI9341, orientation, DC, RST, CS [, BL]
  OPTION LCDPANEL DISABLE
  ```

  On this port the LCD bus is dedicated (SPI3_HOST; SD has its own bus), so
  `OPTION SYSTEM SPI` stores into the `Option.LCD_*` bus fields — same
  user-facing contract, port-appropriate storage. Only `ILI9341` is accepted
  as the controller name until Track B widens the table; the parser is
  written against a name table so B7 is a data change.
- Behavior on accept: validate pins are free (reuse the
  `lcd_profile_pins_available()` check against the Option values), write the
  Option fields, `SaveOptions()`, `_excep_code = RESET_COMMAND; SoftReset()`
  — identical lifecycle to `OPTION SDCARD`. `DISABLE` zeroes the fields.
- `OPTION LIST` prints the LCDPANEL line when configured (extend
  `printoptions` via the existing per-subsystem print hooks).
- Boot pin reservation: `esp32_board_profile_reserve_pins()` switches from
  profile fields to the Option fields it now mirrors.

Exit: on a generic ESP32-S3 devkit, an ILI9341 wired to arbitrary free GPIOs
comes up via the two OPTION commands; `OPTION RESET FREENOVE ILI9341` still
yields a working Freenove out of the box.

### A3 — touch follows the same pattern

- FT6336U I2C pins (currently `profile->touch.*`,
  `esp32_board_profile.h:47-50`) move to Option-backed storage
  (extension slots if no struct fields fit) with
  `OPTION TOUCH sda, scl, int [, rst]` and `OPTION TOUCH DISABLE`.
- `esp32_ft6336u_touch_init()` reads the Option values; profile seeds them.
- Calibration (`OPTION TOUCH CALIBRATE`) is already Option-backed and
  unchanged.

A3 may land separately from A1+A2; it shares no files with them beyond the
profile seeder.

### Track A validation

- `buildesp32.sh all` clean (octal + quad).
- Host `./run_tests.sh` full pass (should be trivially unaffected — no
  shared files change; a failure means scope leaked).
- Hardware: Freenove board smoke (text console, FASTGFX demo, touch),
  then the generic-wiring test — same panel, different GPIOs, configured
  purely via OPTION.

---

## Track B — transport-neutral SPI LCD controller core

Goal: the panel knowledge in `drivers/spi_lcd/spi_lcd.c` (controller tables,
init sequences, address windows, orientation/MADCTL, inversion policy)
compiles on any port; the per-MCU bus mechanics live behind a small
transport contract. PicoMite behavior is byte-for-byte unchanged; the ESP32
port swaps its hand-rolled ILI9341 command path for the shared core and
gains the controller family.

The layer boundaries, risks, and non-goals from the original sketch of this
plan hold; the phases below make them concrete.

### B0 — inventory and naming hygiene

- Catalog the symbols `spi_lcd.c` exports that other TUs consume
  (`DrawRectangleSPI`, `DrawBitmapSPI`, `spi_write_*`, `DefineRegionSPI`,
  `InitDisplaySPI`, `ConfigDisplaySPI`, …) and which are controller-level
  vs transport-level vs presentation-level.
- Rename any real ESP32 framebuffer implementation still living in a
  `_stub.c`-named file.
- No behavior change; this phase produces a symbol map in this doc.

### B1 — transport contract: `hal/hal_spi_lcd_bus.h`

Pure contract per the HAL standard (no target macros, no port-config
ifdefs):

```c
void hal_spi_lcd_bus_init(uint32_t hz, int cpol, int cpha);
void hal_spi_lcd_bus_dc(int level);        /* data/command line */
void hal_spi_lcd_bus_cs(int level);
void hal_spi_lcd_bus_write(const uint8_t * buf, size_t len);
void hal_spi_lcd_bus_write_pixels(const uint8_t * buf, size_t len);
void hal_spi_lcd_bus_wait_idle(void);
/* optional, capability-gated: */
int  hal_spi_lcd_bus_read(uint8_t cmd, uint8_t * buf, size_t len);
```

- Hot-path note: `write_pixels` is the only bandwidth-critical entry; the
  pico implementation keeps the PL022 FIFO loop and `__not_in_flash_func`
  placement. Per the plan-wide rule, binding is link-time — no function
  pointers added to pixel paths beyond the `lcd_xmit_byte_multi` indirection
  that already exists.
- Implementations: `drivers/spi_lcd/spi_lcd_bus_pico.c` (verbatim transplant
  of `spi_write_fast`, `spi_finish`, the `spi_init`/`spi_set_format` bring-up,
  and the CD/CS GPIO helpers) and `ports/esp32_s3/main/spi_lcd_bus_esp32.c`
  (wrapping the existing `spi_master` transaction helpers from
  `esp32_ili9341_lcd.c`).
- Validation here is compile-only on the pico side: call sites still in
  `spi_lcd.c`, now routed through the contract; all device variants build;
  host suite green (links the stub or nothing — host has no SPI LCD).

### B2 — controller core extraction

- New `drivers/spi_lcd/spi_lcd_panels.c`: `display_details[]`, every
  controller init sequence, `spi_write_command/data/cd/CommandData`,
  `DefineRegionSPI`, orientation/MADCTL handling, `ResetController`.
  Transplant verbatim — every branch and quirk moves as-is; cleanups are
  separate commits after tests pass.
- The 29 `gpio_put(LCD_CD_PIN, …)` sites become `hal_spi_lcd_bus_dc()`;
  `SetCS`/`ClearCS` become `hal_spi_lcd_bus_cs()`; the file's pico includes
  drop to zero. Purity gate extends to cover it.
- What remains in `spi_lcd.c` (pico-only): the fast blit/draw paths
  (`DrawRectangleSPI`, DMA, MEM332 hooks), which consume the same contract
  plus pico-specific acceleration.
- Validation: PicoMite ILI9341 + ST7789-class smoke on hardware;
  `buildall.sh` all variants; host suite; byte-identical init traffic is the
  review bar for the transplant.

### B3 — ESP32 adopts the controller core

- `esp32_ili9341_lcd.c` sheds its command/init/window code and calls
  `spi_lcd_panels` entry points through the ESP32 bus adapter; it keeps the
  port-specific presentation machinery (PSRAM shadow buffer, dirty tiles,
  flush task).
- `OPTION LCDPANEL` controller-name table widens from {ILI9341} to the
  panels validated on ESP32 hardware — gated per panel, not wholesale:
  start ILI9341 + one ST77xx-class device, extend as boards are tested.
- Validation: Freenove regression (text, touch, FASTGFX, FRAMEBUFFER), one
  non-ILI9341 panel on a generic devkit.

### B4 — shared presentation + framebuffer/FASTGFX engines (follow-on)

The remaining duplication — RGB121→RGB565 expansion, dirty-rect
presentation, `FRAMEBUFFER`/`FASTGFX` command semantics — is a separate
campaign with its own risks (Pico core1 merge timing, ESP32 PSRAM
bandwidth). It is intentionally out of scope for B1–B3; the original layer
sketch (generic display HAL, presentation backend, common engines) remains
the direction when it's picked up. B1–B3 deliver the user-visible goal
(any panel, any pins, both MCU families) without touching the merge
pipelines.

### Track B risks (carried forward, still accurate)

- Performance regression on Pico if transport abstraction adds per-pixel
  overhead — held off by keeping `write_pixels` as the existing FIFO loop
  and link-time binding.
- Controller init tables may hide board-specific quirks currently coupled
  to option handling — the verbatim-transplant rule plus per-panel gating
  on ESP32 contains this.
- Readback differs by panel and wiring — `hal_spi_lcd_bus_read` is
  capability-gated, never assumed.
- Multiple SPI LCD families selectable in one firmware already works on
  PicoMite via `Option.DISPLAY_TYPE` runtime dispatch; the extraction keeps
  that dispatch, so no new dispatcher is needed.

## Sequencing and exit gates

| Step | Scope | Gate |
|---|---|---|
| A1 | ESP32 drivers read Option | esp32 builds; Freenove smoke; no `profile->lcd` reads outside seeder |
| A2 | OPTION LCDPANEL / SYSTEM SPI on ESP32 | generic-wiring hardware test; OPTION LIST shows config |
| A3 | OPTION TOUCH pins | touch on generic wiring |
| B0 | symbol inventory | doc updated, no code change |
| B1 | bus contract + pico/esp32 adapters | buildall + esp32 builds + host suite; purity gate |
| B2 | controller core extraction | PicoMite hardware regression; byte-identical init |
| B3 | ESP32 adopts core | Freenove regression + one new panel |
| B4 | presentation/engine unification | separate campaign, own plan refresh |

Every step holds the standing invariants: host `./run_tests.sh` full pass
(never lower than the current count), `buildall.sh` clean on all device
variants, `buildesp32.sh all` clean, `tools/check_hal_purity.sh` green.
