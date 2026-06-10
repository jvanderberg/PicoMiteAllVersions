# Retire `ports/pico_sdk_compat/` — SDK calls out of core and shared

## Goal

Every line of core/ and shared/ compiles against `hal/*.h` contracts only — no
Pico SDK headers, no `_hw->` register access, no vendor function calls. The
campaign's terminal gate — **`ports/pico_sdk_compat/` and
`ports/host_native/pico/` deleted, with every non-Pico build (host_native,
host_wasm, pc386, mmbasic_stdio, mmbasic_ansi, esp32_s3) still building and
passing its suite** — **is reached: the shim directories are gone** (Phase 7).

That terminal gate was the point. The shim directories existed so that shared
code could keep calling `adc_read()` or poking `systick_hw->cvr` and still
compile everywhere — on non-Pico ports those calls silently did nothing
(`adc_read()` returned 0). Deleting the shims converted every remaining vendor
call site into a hard build error, so the finish line could not be gamed:
either the call moved behind a HAL contract with a real backend or an erroring
stub, or the build went red. From here, reintroducing an SDK call in core/ or
shared/ is both a build error on six ports and a `check_hal_purity.sh`
failure.

This extends `peripheral-io-hal-plan.md` (whose Step 11 sweep this campaign
absorbs) and follows the same architecture principle: **one shared body for
command + syscall logic; hardware specifics exist only inside `hal_*` port
backends.** If a line of shared code names a Pico-SDK call, it is in the
wrong place.

## The standard, extended

`real-hal-plan.md` defines HAL-clean as zero target-macro and port-config
preprocessor gates. This campaign adds the next ratchet for core/ and shared/:

5. **Zero `#include` of `hardware/*` or `pico/*` headers.**
6. **Zero `_hw->` register access and zero Pico-SDK / vendor function calls.**

`tools/check_hal_purity.sh` enforces both since Phase 7 (the SDK-clean
section: every file under core/ and shared/ is checked for SDK includes,
`_hw->` register-window tokens, and `cyw43_*` calls). During the campaign the
scoreboard below was the ratchet: a file's include count could only decrease,
and a phase closed only when its targeted files reached zero.

## Scoreboard — SDK include lines per file

Baseline measured 2026-06-09 on `main` (post GPIO/I²C/PWM/SERVO unification).

| File | Baseline | Now | Phase |
|---|---|---|---|
| `core/mmbasic/External.c` | 13 | 0 | 0–5, 7 ✅ |
| `core/mmbasic/MM_Misc.c` | 14 | 0 | 1, 2, 3, 5, 7 ✅ |
| `core/mmbasic/Custom.c` | 10 | 0 (file relocated to `drivers/pio_rp2/`) | 5 ✅ |
| `core/mmbasic/XModem.c` | 1 | 0 | 0 ✅ |
| `core/mmbasic/Draw.h` | 1 | 0 | 0 ✅ |
| `shared/net/MMsetwifi.c` | 3 | 0 | 6 ✅ |
| `shared/net/MMtelnet.c` | 1 | 0 | 6 ✅ |
| `shared/net/MMtftp.c` | 1 | 0 | 0 ✅ |
| `shared/net/MMntp.c` | 1 | 0 | 0 ✅ |
| `shared/net/MMweb_stubs.c` | 1 | 0 | 4 ✅ |
| **Total** | **46** | **0** | |

## Current state (verified) — what the shims still carry

Per-function inventory of vendor call sites, grouped by the contract that
absorbs them:

| Domain | Call sites | Disposition |
|---|---|---|
| **Dead code** | `External.c set_PWM()` (lines ~1854–2026, 36 SDK calls) — no declaration in any header, no callers; orphaned when the PWM write path moved to `vm_sys_pwm.c` + `hal_pwm.h` | Delete |
| **PWM readback/teardown** | `MM_Misc.c fun_info` reads `pwm_hw->slice[].top/cc` (MM.INFO duty/frequency); `External.c PWMoff` + `ClearExternalIO` PWM teardown; `External.c setBacklight` drives the backlight slice directly | Extend `hal_pwm.h` with a query call; route teardown through `hal_pwm_stop`; route backlight through the existing backlight hook |
| **ADC** | `External.c ExtInp` (single conversion incl. `adc_hw->cs` error-sticky handling), `fun_pin`, `SetADCFreq`, `cmd_adc` capture sessions with chained DMA, `ADCint` ISR tail, `MM_Misc.c checkdetailinterrupts` ADC-completion polling, `ClearExternalIO` teardown | New `hal_adc.h`: init/clock, single read, capture-session API (start/poll/stop with a completion callback). DMA chaining is backend detail. Pin capability validation stays in `hal_pin.h` (`hal_pin_adc_validate`) |
| **Cycle-accurate timing** | `External.c serialtx`/`serialrx` bit-bang off `systick_hw->cvr`; `MM_Misc.c fun_info` systick read | Grow `hal_fast_timer.h` into a cycle-counter contract (read + calibrated busy-wait); the bit-bang bodies stay shared |
| **PIO** | `Custom.c` wholesale (`pio_sm_*`, `dma_*`; the file is the PIO module and carries 37 `rp2350` preprocessor gates); `MM_Misc.c checkdetailinterrupts` PIO/DMA-interrupt polling | Relocate: the machinery moves to `drivers/pio_rp2/`; `AllCommands.h` rows keep pointing at shared thin entry points that call the driver, and non-RP ports link the erroring stub per the stub-driver posture |
| **Console UART raw access** | `XModem.c` (6 `uart_*` calls: IRQ enable/disable around transfers, raw getc/putc, readable poll) | Small raw-mode surface on the serial-console contract (rx-ready, raw getc with deadline, raw putc, irq mask) with per-port backends |
| **Board switches** | `External.c bb_get_bootsel_button` (BOOTSEL via `ioqspi_hw`/`sio_hw`); `External.c SoftReset` watchdog reset; `MMweb_stubs.c` GPIO 23 SMPS power-mode (`Option.PWM`) | BOOTSEL becomes a board-button hook (0 on ports without one); SoftReset routes through `hal_watchdog.h`; the SMPS pin moves into the Pico port |
| **WiFi chip bring-up** | `MMsetwifi.c` (`cyw43_arch_init`, `cyw43_set_pio_clock_divisor` from `clock_get_hz`); `MMtelnet.c` (`cyw43_arch_poll`, activity LED via `cyw43_arch_gpio_*`) | Chip bring-up and poll move behind net-driver hooks; the LED becomes an activity hook. Land after the contract shape in `network-core-plan.md` is settled so the hooks sit where that design wants them |
| **Trivia** | `Draw.h PinRead` macro calls `gpio_get`; `MMtftp.c sleep_us`; `MMntp.c` includes `pico/time.h` without using it | Route `PinRead` through the per-port `hal_pin` fast-read inline; `sleep_us` → `hal_time_sleep_us` (precedent: `mm_net_ntp_hal.c`); drop the dead include |

## Phases

Each phase lands as one batch: edits, then one `tools/validate_all.sh` run at
batch end, plus `buildesp32.sh` / pc386 where the port is touched. Extractions
are verbatim transplants — behavior-preserving, no re-derivation;
simplification is a separate commit after tests pass.

### Phase 0 — dead code and one-liners (no new contracts) — ✅ CLOSED

1. Delete `set_PWM` from External.c (dead: no header declaration, no callers).
2. Drop the unused `pico/time.h` include from MMntp.c.
3. MMtftp.c: `sleep_us(1000)` → `hal_time_sleep_us(1000)`; drop the include.
4. Draw.h: `PinRead` → the existing `hal_pin` fast-read inline (hot path —
   per-port `hal_pin_inlines.h` keeps it a register read on RP).
5. XModem.c: add the console raw-mode hooks and route the 6 `uart_*` calls.
   Backends: RP implements over uart0/uart1 per `Option.SerialConsole`; other
   ports map to their console byte stream or return not-supported (XMODEM
   over the host console already has no meaning there).

Exit: XModem.c, Draw.h, MMtftp.c, MMntp.c at zero. Scoreboard 46 → 42.

Closed at 46 → 38: deleting `set_PWM` also freed four External.c includes
(`hardware/clocks.h`, `hardware/structs/pwm.h`, `hardware/structs/pads_bank0.h`,
`hardware/sync.h`). The raw-mode contract landed as `hal/hal_serial_console.h`
(device backend `ports/pico_sdk_common/hal_serial_console_pico.c`; pc386 stubs
keep transfers on the buffered console). The XMODEM transfer engine's abort
paths route through `xmodem_error()`, which restores the console RX interrupt
before `error()` longjmps — a failed transfer no longer silences a UART
console. `ports/pico_sdk_compat/hardware/uart.h` is now consumer-free.
MMntp.c additionally moved `time_us_64()` → `hal_time_us_64()` (its include
carried that one live symbol). `validate_all.sh` green incl. all 14 device
variants + RAM baseline; pc386 builds; pinned clang-format clean.

### Phase 1 — PWM closeout — ✅ CLOSED

1. Extend `hal_pwm.h` with a readback call (top/duty per channel) and route
   `MM_Misc.c fun_info`'s `pwm_hw->slice[]` reads through it.
2. Route `PWMoff` / `ClearExternalIO` PWM teardown through `hal_pwm_stop`
   (the camera driver's `PWMoff` use follows the same path).
3. Route `setBacklight` through the backlight hook (the ESP32 port already
   owns one; the RP backend keeps the slice math).
4. Route `fun_info`'s `spi_get_baudrate` through a system-SPI query hook next
   to the existing `port_system_lcd_spi_option_setter` family.

Exit: zero `pwm_*` / `spi_get_baudrate` vendor calls in core/.

Closed: `hal_pwm_query` (top + both compare levels, raw backend ticks) in
all four backends; teardown converged on `hal_pwm_stop`, which also fixes a
stale-state bug — a slice torn down at a program boundary stayed marked
started, so a later bare `PWM SYNC` could re-enable it with its pads already
released. The fast-timer wrap-IRQ ack moved into the fast-timer backend (a
RAM-resident trampoline acks then calls the registered handler). Backlight
math landed verbatim as `hal/hal_display_backlight.h` +
`hal_display_backlight_pico.c`; frequency policy (explicit argument /
per-display default) stays with the callers. MM.INFO(SPI SPEED) routes
through `port_mminfo_system_spi_speed` beside its `port_mminfo_*` siblings.
External.c 9 → 8 includes, MM_Misc.c 14 → 10 (one was a duplicate);
zero `pwm_*`/`spi_get_baudrate` symbols anywhere in core/.

### Phase 2 — `hal_adc.h` — ✅ CLOSED

Contract: `hal_adc_init`, `hal_adc_set_clock`, `hal_adc_read(pin_gp)` (single
conversion, error-sticky recovery inside the backend), and a capture-session
API — configure channels/frequency/buffers, start, completion query, stop —
that hides the DMA chaining entirely. Backends: RP real (current External.c
machinery moves there verbatim); ESP32 real where IDF oneshot/continuous
drivers fit, erroring stub otherwise; host/pc386 erroring stubs.

Routes: `ExtInp`, `fun_pin`, `SetADCFreq`, `cmd_adc`, `ADCint`,
`checkdetailinterrupts`'s ADC-completion poll, `ClearExternalIO` teardown.

Exit: zero `adc_*` / `adc_hw` / ADC-DMA references in core/. On ESP32 an
analog `PIN()` read returns a real conversion or a real error — never a
silent 0.

Closed: `hal/hal_adc.h` carries the sample-clock model (set / set-default /
restore-default, replacing the `adc_clk_div` boot-snapshot global), the
single conversion with sticky-error recovery, and the capture-session
surface — continuous double-buffered streaming (the chained-DMA
pointer-rewrite trick and the DMA_IRQ_1 ISR moved verbatim into
`hal_adc_pico.c`; core supplies a RAM-resident buffer-swap callback) plus
single-shot capture with a polled completion that stops and drains the
converter. Raw→MMFLOAT scaling, pin bookkeeping, and interrupt dispatch
stay in core. Teardown converged on `hal_adc_capture_end`. The
`SetADCFreq` shim and the `ADC_dma_chan`/`adcint*` compat globals died
across all ports. vm_sys_pin's ADC cache now stores the BASIC pin number,
restoring the interpreter's re-select-skip invariant. ESP32's analog-read
honesty item rides the existing `hal_pin_adc_*` surface (vm_sys_pin), which
that port already implements; External.c's interpreter path is RP-only.
External.c 8 → 6 includes, MM_Misc.c 10 → 9 (`hardware/dma.h` stays in
both pending the PIO phase).

### Phase 3 — cycle counter — ✅ CLOSED

1. Grow `hal_fast_timer.h` into a cycle-counter contract: read-current,
   calibrate, and a busy-wait primitive precise enough for bit-banged serial.
2. Route `serialtx` / `serialrx` (the SERIN/SEROUT bit-bang) and
   `fun_info`'s systick read through it. The bit-bang bodies stay shared;
   only the time source is port-owned.

Exit: zero `systick_hw` references in core/.

Landed as a sibling header, not a `hal_fast_timer.h` extension: the fast
timer is a PWM-slice event counter while this is a CPU-cycle countdown, and
the consumers run as `__not_in_flash_func` so the surface had to be Tier-B
inline. `hal/hal_cycle_counter.h` + per-port `hal_cycle_counter_inlines.h`
(`hal_cycle_reload` / `hal_cycle_restart` / `hal_cycle_remaining`) keep the
countdown-compare bodies verbatim; the `shortpause()` macro, DEVICE
BITSTREAM, the Touch.c `TDelay`, and MM.INFO(SYSTICK) ride the same surface,
and the SysTick RVR constant (16777215) left core with it.

### Phase 4 — board switches

1. BOOTSEL: `bb_get_bootsel_button` moves behind a board-button hook; ports
   without the button return 0.
2. `SoftReset`: `watchdog_enable(1,1)` → the `hal_watchdog.h` reset path.
3. `MMweb_stubs.c` GPIO 23 SMPS power-mode write moves into the Pico port
   (it is Pico-board power-supply behavior, keyed off `Option.PWM`).

Exit: External.c at zero SDK includes. Scoreboard: core/ leaves only
Custom.c + MM_Misc.c's PIO remnant.

Landed: BOOTSEL is `hal_pin_bootsel_pressed()` — the BASIC surface is
PIN(BOOTSEL), so the contract lives in `hal_pin.h`; the QSPI-CS-override
body moved verbatim into `hal_pin_pico.c` (still RAM-resident inside the
FileIO flash-write guard), and every non-RP backend returns false.
SoftReset routes through the new `hal_watchdog_reboot()` (pico backend
keeps `watchdog_enable(1, 1)` verbatim; the generic no-op leaves host's
follow-on spin loop unchanged). MM_Misc.c's `hardware/structs/watchdog.h`
include was consumer-free and is gone — no watchdog SDK usage remains in
core/ or shared/. MMweb_stubs.c reached zero: the PIO pin-reset loop rides
`hal_pin_set_input_enabled` and the GPIO 23 write moved into
`pico_smps_set_pwm_mode()` (pico_boot.c); the rp2350a/AllPins policy stays
with the `hal_pwm_mode_shadow_apply` stub. External.c holds at 2 includes:
`hardware/dma.h` is Phase 5's, and `pico/stdlib.h` is still bound by the
GPIO-IRQ edge constants + `irq_set_priority(IO_IRQ_BANK0)` (Phase 5/6
candidates alongside the pico_gpio_irq contract) and by the raw
`__not_in_flash_func` on on_pwm_wrap_1/bitstream/serialtx/serialrx —
not PORT_RAM_FUNC-routable, because WiFi ports define PORT_RAM_FUNC as
identity while these bodies must stay RAM-resident there.

### Phase 5 — PIO relocation

1. Move Custom.c's PIO machinery (program assembler state, `pio_sm_*` /
   `dma_*` execution, the 37 `rp2350` gates) into `drivers/pio_rp2/`. The
   shared file keeps only the BASIC-dialect surface: token entry points that
   call the driver. Non-RP ports link the erroring stub (stub-driver
   posture: fail loudly).
2. Move `MM_Misc.c checkdetailinterrupts`' PIO/DMA-interrupt polling behind
   a driver-raised completion hook so the interrupt scan stays shared.

Exit: Custom.c and MM_Misc.c at zero SDK includes; the `rp2350` gate count
in core/ drops by 37.

Landed: Custom.c relocated wholesale — the file was 100% PIO, so it moved
verbatim to `drivers/pio_rp2/pio_rp2.c` (token entry points `cmd_pio`,
`fun_pio` and the per-instruction `cmd_jmp`/`cmd_wait`/… family included;
AllCommands.h binds them at link time) and left core entirely, taking all
37 `rp2350` gates and the PIO globals with it. Custom.h keeps only
`closeMQTT` + `TCP_READ_BUFFER_SIZE`. No new stub was needed: Custom.c,
MM_Misc.c and External.c are compiled only by the device CMake build, and
the existing `*_peripheral_stubs.c` files already satisfy the AllCommands.h
token symbols on non-RP ports. Core reaches the hardware through
`drivers/pio_rp2/pio_rp2.h`: `pio_rp2_pending_interrupt()` (the
checkdetailinterrupts probe — dispatch stays in MM_Misc.c, same pattern as
`hal_gui_controls_pending_interrupt`), `pio_rp2_dma_rx_busy()`/`_tx_busy()`
(MM.INFO), and `pio_rp2_teardown()` / `pio_rp2_dma_abort()` (ClearExternalIO
and `port_runtime_abort_dma`, which dropped its `dma_*_chan` externs).
Fixed in transit: the DMA-completion arms disabled the state machine via
`(dma_rx_pio ? pio1 : pio0)`, which picks pio1 when the transfer was set up
on rp2350's pio2 — now the natural-order `pio_rp2_block()` mapping that the
DMA setup itself uses. (`port_pio_for_index` was deliberately not used: its
rp2040 arm is reversed, encoding the FIFO poll loop's table-index space.)
MM_Misc.c holds at 3 includes: `pico/stdlib.h` (GPIO_IRQ_EDGE_* constants
for the pico_gpio_irq calls; `check_sys_clock_khz` for MM.INFO(VALID
CPUSPEED) on SDK 1.x), `hardware/clocks.h` (`check_sys_clock_khz`'s SDK 2.x
home), and `hardware/regs/addressmap.h` (XIP_BASE flash-address math in
LOAD MODULE + MM.INFO) — the consumer-free `pico/bootrom.h` and
`hardware/pio_instructions.h` are gone. External.c holds at 1
(`pico/stdlib.h`, binders unchanged from the Phase 4 note). core/ `rp2350`
gates: 47 → 10; every remaining one is outside this phase's scope (CMM2
token rows in AllCommands.h, memory-layout constants in configuration.h /
MMBasic.h / FileIO.h / Version.h, includes in Hardware_Includes.h) — none
are PIO.

### Phase 6 — WiFi chip isolation — ✅ CLOSED

Blocked on the contract shape in `network-core-plan.md`; land the hooks where
that design puts the link-layer boundary.

1. cyw43 bring-up (`clock_get_hz`-derived PIO divider + `cyw43_arch_init`)
   moves into the CYW43 driver behind a net bring-up hook.
2. `cyw43_arch_poll` and the activity LED in MMtelnet.c move behind
   poll/activity hooks (the LED hook lands next to `hal_heartbeat.h`'s
   pattern: real on CYW43 ports, no-op elsewhere).

Exit: shared/net at zero SDK includes and zero `cyw43_*` calls.

Closed: the boundary slots already existed in `hal_net.h` — bring-up is
`hal_net_init()` (the lwIP backend now runs the verbatim
`cyw43_pio_divider_for_clk_sys` gSPI guard, then `cyw43_arch_init`,
mapping 0 → `HAL_NET_OK`; previously its `hal_net_init` was a trivial OK
with no Pico caller) and the ProcessWeb pump is `hal_net_poll()` (already
`cyw43_arch_poll()` per the network-core design). MMsetwifi.c's lone
`CYW43_LINK_UP` comparison became the backend predicate
`net_lwip_tcpip_link_up()` — status values from `hal_net_tcpip_status()`
are backend-specific, so the "fully up" test belongs beside them. The LED
landed as `hal_heartbeat_led_get/put` with a new
`drivers/heartbeat/heartbeat_cyw43.c` (WiFi ports link it instead of
heartbeat_stub.c; stub/real carry no-ops); blink cadence stays verbatim in
MMtelnet.c's ProcessWeb. MMtelnet.c's `time_us_64` had been declared only
via the removed cyw43 header — moved to `hal_time_us_64()` (MMntp
precedent). shared/ and core/ now grep clean of `cyw43`. Scoreboard
46 → 4 (core/ MM_Misc.c 3 + External.c 1 remain). Host 291/291, purity
green, full WiFi device builds green: WEB, picocalc_wifi_rp2040,
picocalc_wifi_rp2350, DVIWIFIRP2350 (the CYW43_PIO_CLOCK_DIV_DYNAMIC
shape that compiles the moved divider).

### Phase 7 — delete the shims, ratchet the gate — ✅ CLOSED

Prep landed — core/ at zero SDK include lines, scoreboard 46 → 0:

- **RAM placement** (External.c `pico/stdlib.h`, binder 1): the raw
  `__not_in_flash_func` decorations were not PORT_RAM_FUNC-routable —
  no RP variant is a copy-to-RAM build (no `PICO_COPY_TO_RAM` anywhere),
  so identity-PORT_RAM_FUNC ports (WEB, WEBRP2350, VGA/VGAWIFI, the
  WiFi PicoCalcs) place PORT_RAM_FUNC bodies in XIP flash by design.
  New `PORT_TIMING_CRITICAL_FUNC` in the port_config.h family:
  `__not_in_flash_func` on **all 13** RP ports, identity on host/ESP32.
  All ten External.c bodies (on_pwm_wrap_1, bitstream, serialtx/rx, the
  four TM_EXTI handlers, IRHandler, gpio_callback) moved onto it; nm on
  PICO / WEB / PICORP2350 / DVIWIFIRP2350 confirms every one stays in
  SRAM while WEB's PinSetBit/ExtSet/ExtInp stay in flash — placement
  identical to baseline on every shape. RP port_config.h now includes
  `pico.h` so the placement macros expand without core SDK includes.
- **GPIO edge constants** (binder 2, both files):
  `pico_gpio_irq_set_enabled` now takes a HAL_PIN_EDGE_* mask; the
  HAL→SDK event translation (values differ: HAL 1/2 vs SDK 8/4) lives
  in pico_gpio_irq.c. All 16 core sites plus the ps2_mouse/ps2_matrix
  driver callers converted; the counting-input `edge` variable and the
  `hal_pin_irq_set_edge` calls now share one vocabulary, dropping the
  inline conversion expressions.
- **`irq_set_priority(IO_IRQ_BANK0, 0)`** (binder 3):
  `pico_gpio_irq_set_highest_priority()` beside the dispatcher it tunes.
- **`check_sys_clock_khz`** (MM_Misc.c): `port_mminfo_valid_cpuspeed()`
  beside its port_mminfo_* siblings in misc_option_setters.c (which
  includes both the SDK 1.x and 2.x homes of the declaration).
- **`XIP_BASE`** (MM_Misc.c): both sites computed the MOD-buffer CPU
  address (`XIP_BASE + RoundUpK4(TOP_OF_SYSTEM_FLASH)`) — hal_flash.h
  speaks absolute flash offsets, not XIP mappings, so this landed as
  `port_modbuff_address()` in misc_option_setters.c rather than a HAL
  query. gpio_callback's `uint` parameter became `unsigned int`
  (External.h carried the SDK typedef).

Closed — the shim directories are deleted and the gate is ratcheted:

1. `git rm -r ports/pico_sdk_compat ports/host_native/pico` (31 headers);
   include paths dropped from the five port Makefiles,
   `ports/esp32_s3/main/sources.cmake`, and `tools/check_vm_pin_modes.sh`.
2. Deletion fallout, fixed by the campaign's rules (all dead compat code or
   driver-internal relocation — no new shims, no `#if` gates):
   - `drivers/spi_lcd/SPI-LCD.h` (in every core TU via Hardware_Includes.h)
     included `hardware/spi.h` for two `spi_inst_t` fast-path declarations
     (`spi_write_fast` / `spi_finish`, defined in spi_lcd.c). The
     declarations moved into the RP TUs that share them (spi_lcd_mem332,
     spi_lcd_framebuffer, spi_lcd_fastgfx — VS1053.c already carried its
     own), each beside its own `hardware/spi.h` include; the header is now
     SDK-free.
   - `configuration.h`'s `QVGA_PIO` macro named `pio0/pio1/pio2` (flagged by
     the new gate section, not a build break — the SDK defines them on RP).
     It moved to its only consumer, `drivers/vga_pio/vga_qvga_modes.c`,
     taking one `rp2350` gate out of core; `QVGA_PIO_NUM` stays (the VGA
     ports' `piomap[]` bookkeeping keys on it).
   - Dead `dma_hw`/`watchdog_hw` dummy-storage blocks deleted from
     host_runtime.c, pc386_state.c, and esp32_compat.c (no consumer
     anywhere in core/shared/runtime since the ADC/PIO phases), along with
     the consumer-free `port_pio_for_index` stubs in host_runtime.c and
     pc386_runtime.c (its only caller is `drivers/pio_rp2/pio_rp2.c`, RP-only)
     and host_main.c's unused `hardware/flash.h` include.
   - runtime_abort.c / runtime_interrupt.c dropped their
     `__has_include("pico.h")` blocks — redundant on RP since the
     port_config.h family includes `pico.h` itself (Phase 7a), dangling on
     host once `ports/host_native/pico/platform.h` was deleted. Their
     `MMB_HOT_FUNC` comes from port_config.h via MMBasic_Includes.h.
3. `tools/check_hal_purity.sh` grew the SDK-clean section: all of core/ and
   shared/ (not just STRICT_FILES) fails on (a) `#include` of `hardware/*`,
   `pico/*`, or `pico.h`, (b) `_hw->` register-window tokens and the
   pio0/pio1/pio2 instance structs (word-boundary match over
   comment-and-string-stripped source), (c) `cyw43_*` calls. Detection was
   verified by injecting each violation class into a core file (all three
   fail the gate) plus a comment-only mention (passes).
4. Matrix green: purity gate; host_native build + 291/291 tests (incl.
   check_vm_pin_modes); mmbasic_stdio; mmbasic_ansi; pc386 (i686-elf-gcc);
   device PICO, PICORP2350, WEB, VGA, VGARP2350 from clean /tmp trees (the
   five shapes that compile every TU this phase touched). host_wasm built in
   CI (no local emcc; validate_all skips it the same way).

Exit reached: the campaign's terminal gate — the shim directories no longer
exist and every build is green.

## Deferred / out of scope

- `OPTION COUNT` and `OPTION AUDIO` extraction (carried from
  `peripheral-io-hal-plan.md`): not shim-blocking; extract when a port needs
  them or when the audio checkslice hook lands.
- `Hardware_Includes.h` itself: stays — it is the port-composition umbrella,
  not a vendor shim. Pruning it is follow-up hygiene once the SDK includes
  beneath it are gone.
- The RP ports' own SDK usage (`ports/pico_sdk_common/`, `drivers/*_pico*`,
  `drivers/*_rp2*`): correct by design — that is where vendor calls belong.
