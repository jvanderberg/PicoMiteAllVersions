# Peripheral I/O HAL — unify GPIO, I²C, PWM, SERVO across all ports

## Goal

One unified, port-agnostic implementation of **GPIO, I²C, PWM, and SERVO** that
every port shares, with **all** hardware specifics behind HAL contracts
(`hal_pin.h`, `hal_i2c.h`, `hal_pwm.h`). Pins are runtime configuration
(`SETPIN`/`OPTION`); the board profile only supplies overridable defaults. A new
board with different headers works by changing its profile's default pins — no
code change.

This lands as **one PR** covering all four capabilities and every affected port.

## Architecture principle (non-negotiable)

**We do not preserve any port's existing GPIO/I²C/PWM/SERVO code.** Today the
logic is *duplicated per port* — RP drives it from `runtime/vm/vm_sys_pin.c`
(Pico SDK), ESP32 from `vm_sys_pin_esp32.c` (stubs), `I2C.c` calls the Pico SDK
inline, and `cmd_pwm` is copied into every port. That all gets **rewritten into
a single shared implementation.** The rule:

- **One** copy of the command + syscall + driver logic (`cmd_*`, `vm_sys_*`,
  the I²C driver), shared by *all* ports.
- **Hardware-specific code exists only inside `hal_*` port backends.** If a line
  of shared code names a Pico-SDK call (`pwm_set_*`, `i2c_init`,
  `gpio_set_function`) or an ESP-IDF call (`ledc_*`, `i2c_master_*`), it is in
  the wrong place — it moves to that port's `hal_*` backend.
- **No per-port command/syscall files** for these peripherals. `vm_sys_pin.c`
  and `vm_sys_pin_esp32.c` collapse into **one** `vm_sys_pin.c`; the per-port
  `cmd_pwm`/`cmd_setpin`/`cmd_i2c` copies collapse into **one** shared body.
- **No target `#ifdef`s** in the shared code (enforced by the purity gate).
- Existing RP behavior must remain *functionally* correct, but its *code* is not
  sacred — it is extracted, not copied. "It already works on Pico" is not a
  reason to keep Pico-shaped code in the shared layer.

Result: GPIO/I²C/PWM/SERVO differ between RP2040, RP2350, ESP32-S3, host, and
pc386 **only** in their `hal_*` backend `.c` files.

## Current state (verified) — the duplication we are removing

| Capability | Today | Problem |
|---|---|---|
| **GPIO** | `vm_sys_pin.c` (RP, Pico SDK) **and** `vm_sys_pin_esp32.c` (ESP32) — two separate bodies, same signatures | duplicated logic; only the leaf `hal_pin_*` is shared |
| **PWM** | `vm_sys_pwm_configure` etc. implemented twice (RP body does `pwm_set_chan_level`/`pwm_set_enabled` on a slice; ESP32 body errors). `cmd_pwm` copied into pc386/esp32/host ports | duplicated logic + duplicated command parser |
| **SERVO** | `vm_sys_servo_configure` twice (RP real, ESP32 errors) | same |
| **I²C** | shared `drivers/i2c_bus/I2C.c` but it calls Pico SDK inline (`i2c_write_timeout_us`, `i2c_init`, slave via `i2c->hw->` registers + IRQ). ESP32 `cmd_i2c` is a `{}` stub | "shared" driver is actually Pico-bound |

Both execution engines already converge here: the interpreter (`cmd_pwm` →
`vm_sys_pwm_configure`) and the bytecode VM (`OP_SYSCALL` → same
`vm_sys_pwm_configure`) call the identical syscall — so unifying the syscall
body fixes both `RUN` and `FRUN` at once.

## Target architecture

```
BASIC  ──►  cmd_*  ──►  vm_sys_*  ──►  hal_*  ──►  port backend (.c)
(shared)   (shared)    (shared)     (contract)   (RP / ESP32 / host)
                ▲                                    pwm_set_* / ledc_*
   OP_SYSCALL ──┘                                    i2c_*      / i2c_master_*
   (bytecode VM)                                     gpio_*     / driver/gpio
```

Everything left of `hal_*` is one shared copy. Everything in the backend is
per-port and nothing else is.

## HAL contracts

`hal/CONTRACT.md` rules: `int` returns (`0` ok / negative errno), caller owns
buffers, HAL impls never call `error()`, `hal_<name>_init()` exists, headers
pass `tools/check_hal_purity.sh`.

**`hal/hal_pin.h`** — already exists and is the model (GPIO/ADC/pull/function).
The work is to make the *one* shared `vm_sys_pin.c` use it for every port.

**`hal/hal_i2c.h`** (new):
```c
/* bus 0 = I2C/I2C0, bus 1 = I2C2/I2C1 */
int  hal_i2c_master_init(int bus, int sda_gpio, int scl_gpio, uint32_t baud);
void hal_i2c_master_deinit(int bus);
int  hal_i2c_master_write(int bus, uint8_t addr, const uint8_t *buf, size_t len,
                          int nostop, uint32_t timeout_us);   /* >=0 bytes / <0 err */
int  hal_i2c_master_read (int bus, uint8_t addr, uint8_t *buf, size_t len,
                          int nostop, uint32_t timeout_us);
/* slave (RP backend = its current IRQ impl; ESP32 = ESP-IDF i2c slave) */
int  hal_i2c_slave_enable(int bus, uint8_t addr);
int  hal_i2c_slave_poll  (int bus, uint8_t *buf, size_t cap, size_t *len);
int  hal_i2c_slave_send  (int bus, const uint8_t *buf, size_t len);
void hal_i2c_slave_disable(int bus);
```

**`hal/hal_pwm.h`** (new):
```c
int  hal_pwm_init(void);
int  hal_pwm_configure(int channel, int gpio, float freq_hz,
                       float duty_pct, int has_b, float duty_b_pct, int invert);
int  hal_pwm_set_duty(int channel, int which, float duty_pct); /* which 0/1 */
void hal_pwm_stop(int channel);
int  hal_pwm_channels(void);
```

## Per-peripheral unification

**GPIO** — collapse `vm_sys_pin.c` + `vm_sys_pin_esp32.c` into one shared
`vm_sys_pin.c` that calls only `hal_pin_*`. The RP `gpio_*`/ADC specifics and the
ESP32 `driver/gpio`/`esp_adc` specifics move to each port's `hal_pin` backend.

**I²C** — one shared `cmd_i2c`/`cmd_i2c2` + `I2C.c` calling `hal_i2c_*`. The RP
SDK master calls and the IRQ slave code move to `hal_i2c_pico.c`; ESP32 master on
`i2c_master`, slave on ESP-IDF i2c-slave, in `hal_i2c_esp32.c`. Pins via
`SETPIN sda,I2C0SDA` / `SETPIN scl,I2C0SCL`.

**PWM** — one shared `cmd_pwm` + `vm_sys_pwm_*` doing the freq/duty math, calling
`hal_pwm_*`. RP `pwm_set_*` → `hal_pwm_pico.c`; ESP32 LEDC → `hal_pwm_esp32.c`
(`esp32_backlight.c` already drives LEDC — reuse). ESP32 pin table gains PWM
channel caps (drop the blanket `ESP32_NO_PWM`).

**SERVO** — one shared `vm_sys_servo_configure` = `hal_pwm_configure(ch, pin,
50, position→duty)`. No backend work beyond PWM.

## Channel / pin model

- **PWM channels**: RP keeps its fixed slice↔pin mapping inside its backend;
  ESP32 exposes LEDC's 8 channels (any pin, 4 shared timers). The shared layer
  just sees abstract `channel` ids — each backend maps them to its hardware.
- **I²C pins**: always from `SETPIN`/`OPTION`; FREENOVE profile seeds
  `Option.SYSTEM_I2C_SDA=GP16`, `SYSTEM_I2C_SCL=GP15` (overridable). `GENERIC`
  seeds nothing.

## Scope — one PR

GPIO + I²C (master+slave) + PWM + SERVO unification, all backends (RP2040,
RP2350, ESP32-S3, host, pc386), in a single PR. Smoke on the Freenove:
`SETPIN`+`PIN` (GPIO), `I2C OPEN/WRITE/READ` on GP15/16, a `PWM` pin and a
`SERVO` on the IO header.

## Gates (behaviour preserved, code unified)

- `run_tests.sh` stays **192/192**.
- HAL purity gate green; **zero** Pico-SDK / ESP-IDF symbols in shared files.
- Builds clean: ESP32-S3 (octal + quad) **and** at least one RP2040 and one
  RP2350 `.uf2` target, host, pc386.
- RP PWM/I²C/GPIO still *function* identically (re-smoke a Pico target) — but the
  code that drives them now lives in `hal_*_pico.c`, not the shared layer.

## Open decisions

1. **PWM channel model** — LEDC 8 channels exposed as the `PWM` command's
   channels on ESP32; RP keeps slice mapping in-backend. (Recommended.)
2. **Pico backend placement** — `drivers/i2c_bus/hal_i2c_pico.c` +
   `drivers/.../hal_pwm_pico.c` (shared by all RP2 ports) vs per-port dir.
3. **ESP32 I²C slave** — full ESP-IDF i2c-slave in this PR, or a functional
   stub with the slave *architecture* unified now and the ESP32 backend filled
   next. (The architecture is unified either way.)

## Phase 2 — finish the centralization (config front-end extraction)

Phase 1 unified the *driver* (`cmd_i2c`/`I2C.c`) and the *syscall* layers, but
the **configuration front-end** for I²C stayed in two RP-centric core files that
**no non-RP port compiles** — `core/mmbasic/External.c` (the `SETPIN …,I2C0SDA/
SCL/I2C1SDA/SCL` mode dispatch, the `I2C0SDApin/I2C0SCLpin/I2C1SDApin/I2C1SCLpin`
globals, and their `ClearPin` reset) and `core/mmbasic/MM_Misc.c` (the
`OPTION SYSTEM I2C …` set/print/disable). ESP32 only papers over this with stub
globals (`esp32_peripheral_stubs.c`) seeded by the board profile at boot.

Consequence: **a generic ESP32 board cannot configure an I²C bus at runtime** —
`SETPIN gp,I2C0SDA` and `OPTION SYSTEM I2C` are unreachable there. That breaks the
non-negotiable principle ("pins are runtime configuration via SETPIN/OPTION,
shared by all ports"). Phase 2 pulls the remaining Pico-resident config/command
logic into shared units every port compiles, mirroring the
`shared/peripheral/pwm_cmd.c` + `shared/mmbasic/mm_misc_shared.c` precedent.

Same gates as Phase 1 (`run_tests.sh`, purity, all-port builds), plus: a generic
ESP32 must bring up I²C purely via `SETPIN`+`OPTION` with no board profile and no
code change.

**Step 8 — Centralize I²C pin config (SETPIN modes + globals).** Move the four
`EXT_I2C0SDA/SCL/I2C1SDA/SCL` SETPIN case bodies (External.c:957-980, verbatim:
mode-bit check, `I2Cnlocked` guard, "Already Set" guard, pin assignment, the
`hal_pin_set_function(...I2C)` tail), the pin globals (External.c:190-193), and the
`ClearPin` reset-to-99 (External.c:446-449) into a shared unit
(`shared/peripheral/i2c_config.c` or the shared `vm_sys_pin.c`). Route the shared
`vm_sys_pin.c::vm_sys_pin_setpin` I²C-mode path through it so RP, ESP32, host, and
pc386 hit identical logic. Delete the ESP32 stub globals
(`esp32_peripheral_stubs.c`). Verbatim transplant — no re-derivation.

**Step 9 — Centralize `OPTION SYSTEM I2C`.** Move the parse/validate/SLOW/DISABLE
logic (MM_Misc.c:1510-1560), the `OPTION LIST` print (MM_Misc.c:734-735), and
`disable_systemi2c()` (MM_Misc.c:968-975) into a shared option handler wired into
`core/mmbasic/OptionCommands.c::option_command_handle_common()` (already linked by
every port, incl. ESP32). After this, `OPTION SYSTEM I2C sda,scl[,SLOW|FAST]` and
its `DISABLE`/reset work identically on every port. Keep the soft-reset/SaveOptions
behavior intact.

**Step 10 — Migrate all ESP32 I²C off ESP-IDF's deprecated legacy driver to the
new master+slave driver, and wire receive interrupts.** ESP-IDF ships two I²C
driver generations; the legacy `driver/i2c.h` API is deprecated and must not be
used. Today two ESP32 files include it: `ports/esp32_s3/main/hal_i2c_esp32.c`
(master + the slave) and `ports/esp32_s3/main/esp32_freenove_i2c.c` (touch/keypad
bring-up). Move BOTH to the new framework — master via `driver/i2c_master.h`
(`i2c_new_master_bus` / `i2c_master_transmit` / `i2c_master_receive` /
`i2c_master_transmit_receive`), slave via `driver/i2c_slave.h`
(`i2c_new_slave_device`). No `#include "driver/i2c.h"` may remain anywhere in the
ESP32 port. The `hal_i2c.h` contract is unchanged; this is purely the backend
implementation. Wire the slave's `on_recv_done` ISR callback to raise
`I2C_Status_Slave_Receive_Rdy` so a BASIC `I2C SLAVE OPEN`'s **receive** interrupt
subroutine fires on ESP32, matching the RP backend. **Silicon limit (document, do
not paper over):** ESP32-S3 lacks `SOC_I2C_SLAVE_CAN_GET_STRETCH_CAUSE`, so there
is no ISR for a master read-request — the **send** interrupt subroutine cannot
fire on S3; transmit data is pre-loaded via `i2c_slave_transmit`. RP backend
unchanged. Not hardware-validated here (build + purity + RP/host regression only),
including the Freenove touch path which this step rewrites.

**Step 11 — Audit & extract remaining Pico-resident peripheral/option logic.**
Sweep `External.c` and `MM_Misc.c` for other peripheral-config and `OPTION`/`SETPIN`
handling that is still RP-only but architecturally belongs to all ports (other
special-function SETPIN modes, peripheral `OPTION` setters, pin-reservation
bookkeeping). Extract each into the shared layer compiled by every port, verbatim,
so "config is shared, hardware is in `hal_*`" holds with no RP-shaped command logic
left stranded in core files that non-RP ports don't compile. Land incrementally;
gate each extraction on the full build matrix + purity.

**Step 12 — Phase 2 validation.** Full `tools/validate_all.sh` green (incl.
mmbasic_stdio + mmbasic_ansi), `buildesp32.sh all`, pc386, purity. Add a generic-
ESP32 reachability check: with the GENERIC profile (no seeded pins), `SETPIN
gp,I2C0SDA` + `SETPIN gp,I2C0SCL` + `I2C OPEN` must configure and open a bus with
no code change.
