# SPI LCD Generalization Plan

**Status:** Track A (A1–A3) and Track B0–B3 complete on
`jv/spi-lcd-track-a`. A and B3 are hardware-validated on the Freenove
board: the shared PicoMite ILI9341 init sequence drives the panel through
the bus contract. B2 is hardware-validated on a PicoCalc RP2350B (ST7796SP panel:
boot to console, draw, and physical-panel PIXEL readback through the moved
DefineRegionSPI path all correct). Remaining: B4 (presentation/engine
unification, its own campaign) and per-panel widening of the ESP32
controller-name table as hardware becomes available.

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

### B0 — inventory and naming hygiene ✅ (findings below)

The transport survey changed B1's shape. What the code actually does:

- **The Pico transport is a shared-bus arbiter, not a dedicated bus.**
  `SPISpeedSet(device)` (`spi_lcd.c:2395`) multiplexes the system SPI
  between LCD, touch, and SD, reconfiguring speed/format per device and
  selecting among three senders (SPI0 / SPI1 / bit-bang) by pin mapping
  at runtime. `SetCS()` (`spi_lcd.c:1103`) embeds the re-arbitration: it
  calls `SPISpeedSet(Option.DISPLAY_TYPE)` before asserting CS. A one-shot
  `bus_init(hz, cpol, cpha)` contract cannot model this — the contract
  needs claim/release semantics per transaction group.
- **The seam is already exported.** `SetCS`, `ClearCS`, `LCD_CD_PIN`,
  `LCD_CS_PIN`, `lcd_xmit_byte_multi`, `lcd_rcvr_byte_multi` are external
  symbols declared in `SPI-LCD.h`, consumed today by `spi_lcd_fastgfx.c`,
  `spi_lcd_framebuffer.c`, `spi_lcd_mem332.c`, and
  `drivers/gui_touch/Touch.c`. The bus adapter wraps symbols, not statics.
- **Linkage:** `drivers/spi_lcd/spi_lcd.c` is compiled into every device
  variant from the root `CMakeLists.txt` (VGA ports neutralize it with the
  internal `#if !HAL_PORT_IS_VGA` gate); per-port extras come from
  `port_sources.cmake`.
- **Polarity quirks are controller knowledge:** ST7920 inverts CS and
  drives the CD line as its select. They stay with controller code, not
  in the bus contract.
- Symbol classification — controller-level (moves in B2):
  `display_details[]`, init sequences, `spi_write_command/data/cd/
  CommandData`, `DefineRegionSPI`, `ResetController`, orientation/MADCTL.
  Transport-level (stays per-port): `SPISpeedSet`, `HW0/HW1/BitBang`
  senders, `spi_write_fast`/`spi_finish` PL022 fast path, DMA.
  Presentation-level (stays per-port pending B4): `DrawRectangleSPI`,
  `DrawBitmapSPI`, MEM332/fastgfx machinery.

### B1 — transport contract: `hal/hal_spi_lcd_bus.h` ✅

Amended per B0: claim/release semantics, five entries:

```c
void hal_spi_lcd_bus_begin(void);  /* claim + configure bus, assert CS */
void hal_spi_lcd_bus_end(void);    /* deassert CS, release */
void hal_spi_lcd_bus_dc(int data); /* D/C line: 0 = command, 1 = data */
void hal_spi_lcd_bus_write(const uint8_t * buf, size_t len);
int  hal_spi_lcd_bus_read(uint8_t * buf, size_t len); /* 0 = unsupported */
```

- Implementations: `drivers/spi_lcd/spi_lcd_bus_pico.c` (adapter over
  `SetCS`/`ClearCS`/`LCD_CD_PIN`/`lcd_xmit_byte_multi` — the arbiter and
  senders stay where they are) and the ESP32 entry points in
  `esp32_ili9341_lcd.c` (spi_master transactions; CS hardware-managed, so
  begin/end are no-ops and dc() latches the transaction's D/C level).
- **The ESP32 side consumes the contract immediately**: `lcd_cmd` /
  `lcd_data` route through `hal_spi_lcd_bus_dc`/`_write`, so the entire
  ILI9341 init sequence and pixel path exercise the contract on real
  hardware.
- **Pico call sites do NOT migrate in B1.** Routing `spi_lcd.c`'s 29 CD
  sites onto the contract before extraction would churn the same lines
  twice — B2 moves each call site once, rewriting it against the contract
  as it lands in `spi_lcd_panels.c`. Until then the pico adapter is
  compiled into every device variant (link-validated); its consumer
  arrives with B2.
- Hot-path note: per-pixel bandwidth on Pico flows through
  `lcd_xmit_byte_multi` exactly as today; the contract adds one call layer
  on the command path only, nothing on the pixel path.

### B2 — controller core extraction ✅

- New `drivers/spi_lcd/spi_lcd_panels.c`: `display_details[]`, every
  controller init sequence, `spi_write_command/data/cd/CommandData`,
  `DefineRegionSPI`, orientation/MADCTL handling, `ResetController`.
  Transplant verbatim — every branch and quirk moves as-is; cleanups are
  separate commits after tests pass.
- The 29 `gpio_put(LCD_CD_PIN, …)` sites become `hal_spi_lcd_bus_dc()`;
  `SetCS`/`ClearCS` pairs become `hal_spi_lcd_bus_begin()`/`_end()` (ST7920's
  inverted polarity is handled in the controller code that knows about it);
  the file's pico includes drop to zero. Purity gate extends to cover it.
- What remains in `spi_lcd.c` (pico-only): the fast blit/draw paths
  (`DrawRectangleSPI`, DMA, MEM332 hooks), which consume the same contract
  plus pico-specific acceleration.
- Validation: PicoMite ILI9341 + ST7789-class smoke on hardware;
  `buildall.sh` all variants; host suite; byte-identical init traffic is the
  review bar for the transplant.

### B3 — ESP32 adopts the controller core ✅ (init sequences; window/presentation kept port-local)

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

## Goal conformance (final review, 2026-06-09)

Verdict on "a single core LCD driver stack, configured on ESP32-S3 the
same way as on Pico": the **single core stack is real** — an adversarial
re-derivation confirmed the B2 transplant byte-identical, and a sweep
found zero controller-knowledge duplication outside `spi_lcd_panels.c`
(everything remaining per-port is bus transport, presentation, or the
documented ST7920 carve-out). **Configuration parity is syntactic but not
yet total.** Same commands, same argument shapes, same persistence
lifecycle; the differences that remain:

- Controller names: ESP32 accepts `ILI9341` only — by design, each panel
  is enabled when validated on hardware (the shared core already carries
  all of them).
- Orientation: ESP32 accepts `LANDSCAPE` only. The shared init already
  keys MADCTL on `DISPLAY_ORIENTATION`; the gap is the ESP32 presentation
  layer's compile-time 320×240 geometry. Widening = runtime W/H there.
- `INVERT` argument: supported on both (ESP32 added in the final-review
  fix pass; sets `Option.BGR` exactly like Pico).
- Reset pin: ESP32 accepts `0` = no reset line (a superset; Pico requires
  a pin).
- `OPTION SYSTEM SPI` stores to `Option.LCD_*` on ESP32 (dedicated bus)
  vs `Option.SYSTEM_*` on Pico (shared bus) — invisible at the command
  level, by design.
- Touch: converged and hardware-validated on the Freenove board (panel
  init, OPTION LIST in PicoMite form, and live tap coordinates through the
  converged fields). ESP32 uses Pico's exact shape and the same Option
  fields: `OPTION SYSTEM I2C sda, scl [,SLOW]` declares the bus
  (Option.SYSTEM_I2C_*), `OPTION TOUCH FT6336, irq, reset [,click]
  [,threshold]` attaches the controller (Option.TOUCH_IRQ / TOUCH_CS /
  TOUCH_Click / TOUCH_CAP / THRESHOLD_CAP). The ESP32-private extension
  slots are gone. Remaining delta: ESP32 is capacitive-only (no resistive
  XPT2046 hardware path).

## Known gaps (not in any step's scope yet)

- ~~`OPTION AUDIO FREENOVE`~~ **converged**: `OPTION AUDIO ES8311 bclk,
  ws, dout [,mclk [,ampen [,AMPLOW]]]` with the control bus from OPTION
  SYSTEM I2C; the codec register recipe is MCU-neutral in
  `drivers/es8311/` behind a 3-function bus struct; the Freenove profile
  seeds the fields. Hardware-validated (codec probe id=0x83, audible tone
  through the amp via the AMPLOW polarity slot). No board names remain
  anywhere in the persisted option surface.

- **ESP32 SPI fast path.** The dominant per-transaction cost in spi_master
  is bus arbitration around every polling transmit. Two bypass rungs, both
  entirely inside the ESP32's hal_spi_lcd_bus implementation: (1)
  `spi_device_acquire_bus()` in `hal_spi_lcd_bus_begin()` / release in
  `_end()` — batches a whole command sequence or dirty-tile flush under one
  acquisition, ~30 lines, IDF-sanctioned; (2) raw GPSPI register driving
  per the S3 TRM (FIFO-feed small writes, hand-built DMA descriptors) —
  the true Pico-PL022 analogue, only worth it if profiling shows the
  glyph-rate console / DrawBufferFast paths still bottlenecked after (1).
  The shadow + dirty-tile architecture already amortizes most of the tax
  on bulk flushes.

- `MM.INFO$(LCDPANEL)` is not wired in the ESP32 port's MM.INFO dispatch —
  returns 0 with a panel bound. `OPTION LIST` is the authoritative readout.
  Wire it when B3 widens the controller table (the answer becomes the
  controller name, same as PicoMite).
- The web console persists `DISP_USER` into `Option.DISPLAY_TYPE` across
  saves while active. Display init and pin reservation now key off the
  configured control pins instead of `DISPLAY_TYPE`, which makes the local
  panel immune to that clobbering; a cleaner separation (web console state
  out of the persisted panel field) is B3-adjacent cleanup.

## Sequencing and exit gates

| Step | Scope | Gate |
|---|---|---|
| A1 ✅ | ESP32 drivers read Option | esp32 builds; Freenove smoke; no `profile->lcd` reads outside seeder |
| A2 ✅ | OPTION LCDPANEL / SYSTEM SPI on ESP32 | generic-wiring hardware test; OPTION LIST shows config |
| A3 ✅ | OPTION TOUCH pins | touch on generic wiring |
| B0 ✅ | symbol inventory | doc updated, no code change |
| B1 ✅ | bus contract + pico/esp32 adapters | buildall + esp32 builds + host suite; purity gate |
| B2 ✅ | controller core extraction | hardware-validated on PicoCalc RP2350B (ST7796SP init + panel readback); transplant mechanically byte-identical |
| B3 | ESP32 adopts core | Freenove regression + one new panel |
| B4 | presentation/engine unification | separate campaign, own plan refresh |

Every step holds the standing invariants: host `./run_tests.sh` full pass
(never lower than the current count), `buildall.sh` clean on all device
variants, `buildesp32.sh all` clean, `tools/check_hal_purity.sh` green.
