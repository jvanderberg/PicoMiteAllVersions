# Retire `ports/pico_sdk_compat/` — SDK calls out of core and shared

## Goal

Every line of core/ and shared/ compiles against `hal/*.h` contracts only — no
Pico SDK headers, no `_hw->` register access, no vendor function calls. The
campaign ends when **`ports/pico_sdk_compat/` and `ports/host_native/pico/`
are deleted and all six non-Pico builds (host_native, host_wasm, pc386,
mmbasic_stdio, mmbasic_ansi, esp32_s3) still build and pass their suites.**

That terminal gate is the point. The shim directories exist so that shared
code can keep calling `adc_read()` or poking `systick_hw->cvr` and still
compile everywhere — on non-Pico ports those calls silently do nothing
(`adc_read()` returns 0). Deleting the shims converts every remaining vendor
call site into a hard build error, so the finish line cannot be gamed: either
the call moved behind a HAL contract with a real backend or an erroring stub,
or the build is red.

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

`tools/check_hal_purity.sh` is extended in the final phase to enforce both,
once the count is zero. Until then the scoreboard below is the ratchet:
a file's include count may only decrease, and a phase closes only when its
targeted files reach zero.

## Scoreboard — SDK include lines per file

Baseline measured 2026-06-09 on `main` (post GPIO/I²C/PWM/SERVO unification).

| File | Baseline | Now | Phase |
|---|---|---|---|
| `core/mmbasic/External.c` | 13 | 5 | 0–4 |
| `core/mmbasic/MM_Misc.c` | 14 | 8 | 1, 2, 3, 5 |
| `core/mmbasic/Custom.c` | 10 | 10 | 5 |
| `core/mmbasic/XModem.c` | 1 | 0 | 0 ✅ |
| `core/mmbasic/Draw.h` | 1 | 0 | 0 ✅ |
| `shared/net/MMsetwifi.c` | 3 | 3 | 6 |
| `shared/net/MMtelnet.c` | 1 | 1 | 6 |
| `shared/net/MMtftp.c` | 1 | 0 | 0 ✅ |
| `shared/net/MMntp.c` | 1 | 0 | 0 ✅ |
| `shared/net/MMweb_stubs.c` | 1 | 1 | 4 |
| **Total** | **46** | **28** | |

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

### Phase 6 — WiFi chip isolation

Blocked on the contract shape in `network-core-plan.md`; land the hooks where
that design puts the link-layer boundary.

1. cyw43 bring-up (`clock_get_hz`-derived PIO divider + `cyw43_arch_init`)
   moves into the CYW43 driver behind a net bring-up hook.
2. `cyw43_arch_poll` and the activity LED in MMtelnet.c move behind
   poll/activity hooks (the LED hook lands next to `hal_heartbeat.h`'s
   pattern: real on CYW43 ports, no-op elsewhere).

Exit: shared/net at zero SDK includes and zero `cyw43_*` calls.

### Phase 7 — delete the shims, ratchet the gate

1. `git rm -r ports/pico_sdk_compat ports/host_native/pico`.
2. Drop the include paths from the six build files
   (`ports/{host_native,host_wasm,pc386,mmbasic_stdio,mmbasic_ansi}/Makefile`,
   `ports/esp32_s3/main/sources.cmake`) and `tools/check_vm_pin_modes.sh`.
3. Extend `tools/check_hal_purity.sh`: reject `#include` of `hardware/*` or
   `pico/*` and `_hw->` register access anywhere in core/ and shared/.
4. Full matrix green: `validate_all.sh`, `buildall.sh`, `buildesp32.sh all`,
   pc386.

Exit: the campaign's terminal gate — the shim directories no longer exist
and every build is green.

## Deferred / out of scope

- `OPTION COUNT` and `OPTION AUDIO` extraction (carried from
  `peripheral-io-hal-plan.md`): not shim-blocking; extract when a port needs
  them or when the audio checkslice hook lands.
- `Hardware_Includes.h` itself: stays — it is the port-composition umbrella,
  not a vendor shim. Pruning it is follow-up hygiene once the SDK includes
  beneath it are gone.
- The RP ports' own SDK usage (`ports/pico_sdk_common/`, `drivers/*_pico*`,
  `drivers/*_rp2*`): correct by design — that is where vendor calls belong.
