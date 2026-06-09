# Peripheral I/O HAL — I²C, PWM, SERVO, GPIO (driving port: ESP32-S3)

## Goal

Give the ESP32-S3 port working **I²C**, **PWM**, and **SERVO**, and do it the
way the rest of MMBasic is built: the *driver/command* is port-agnostic, the
*pin mapping* is runtime configuration (`SETPIN` / `OPTION`), and the board
profile only supplies overridable defaults. A new board with different headers
must work by changing its profile's default pins — **no code change**.

The enabling work is a **proper HAL contract** for each peripheral
(`hal/hal_i2c.h`, `hal/hal_pwm.h`) so the shared driver calls the HAL and each
port supplies the backend. No Pico-isms in shared code; no target `#ifdef`s.

## Current state (verified)

| Capability | State | Detail |
|---|---|---|
| **GPIO in/out/analog** | **works** | `vm_sys_pin_setpin` handles `DOUT`/`DIN`/`ARAW` (+ pull-ups) via `hal_pin_*`; pin table marks every GP digital/analog-capable. `SETPIN GP2,DOUT : PIN(GP2)=1` and `PIN(GP14)` work today. |
| `PORT`, `PULSE` | stubbed | `cmd_port`/`cmd_pulse` are `{}` — multi-pin helpers, out of scope here. |
| **PWM** | stubbed | `vm_sys_pwm_configure`/`_sync`/`_off` call `error("PWM not supported …")`. Pin table has `ESP32_NO_PWM` (99) for every pin. `cmd_pwm` body exists and is fine. |
| **SERVO** | stubbed | `vm_sys_servo_configure` errors. Built on PWM. |
| **I²C** | stubbed | `cmd_i2c`/`cmd_i2c2` are `{}` no-ops (registered tokens → silent no-op). The real driver is shared `drivers/i2c_bus/I2C.c`, but it calls Pico SDK directly. |

## The MMBasic pin model (what "config-driven" means here)

Pins are bound at runtime against a per-pin **capability table** (`PinDef[].mode`
bitmap) and validated by `SETPIN`:

- **GPIO**: `SETPIN pin, DOUT|DIN|...` → `vm_sys_pin_setpin`.
- **I²C**: `SETPIN sda, I2C0SDA` + `SETPIN scl, I2C0SCL` set the `I2C0SDApin`/
  `I2C0SCLpin` globals (`core/mmbasic/External.c`); `I2C OPEN` then brings the
  bus up on those pins. Defaults seed from `Option.SYSTEM_I2C_SDA/SCL`.
- **PWM**: `SETPIN pin, PWM…` binds a pin to a PWM channel; `PWM ch,freq,duty`
  drives it. `SERVO ch,pos` is PWM at 50 Hz.

So per peripheral the ESP32 work is: (a) add capability flags to the ESP32 pin
table, (b) handle the mode in `setpin`, (c) implement the HAL backend, (d) seed
FREENOVE defaults (overridable).

## HAL convention (from `hal/CONTRACT.md`)

- Return `int`: `0` ok, negative errno-style. No-fail funcs return the value.
- Caller owns all buffers; the HAL impl allocates only its own scratch at init.
- HAL impls **never** call MMBasic `error()` — they return a code; the caller
  (`cmd_*`) maps it to the BASIC-visible message.
- `hal_<name>_init()` exists; hot paths use the Tier-B `hal_<name>_inlines.h`
  mechanism (I²C/PWM are not hot-path → plain extern is fine).
- Headers in `hal/*.h` must pass `tools/check_hal_purity.sh` (no target macros).

There is already `hal/hal_pin.h` (GPIO, in use). There is **no** general
`hal/hal_i2c.h` (only `hal_i2c_keypad.h`, a specific surface) and no
`hal/hal_pwm.h` — this plan adds them.

## 1. `hal/hal_i2c.h` — I²C master contract

```c
/* bus 0 = I2C / I2C0, bus 1 = I2C2 / I2C1. */
int  hal_i2c_master_init(int bus, int sda_gpio, int scl_gpio, uint32_t baud);
void hal_i2c_master_deinit(int bus);
int  hal_i2c_master_write(int bus, uint8_t addr, const uint8_t *buf, size_t len,
                          int nostop, uint32_t timeout_us);  /* >=0 bytes, <0 err */
int  hal_i2c_master_read (int bus, uint8_t addr, uint8_t *buf, size_t len,
                          int nostop, uint32_t timeout_us);
```

The shared driver's **master** paths (`I2C OPEN/WRITE/READ/CLOSE` and the
register-read helpers) call these. `nostop` = repeated-start (hold the bus);
`timeout_us` mirrors the existing `I2C_Timeout`.

### Refactor (makes `drivers/i2c_bus/I2C.c` port-agnostic)
1. Add `hal/hal_i2c.h` (purity-clean).
2. Replace the Pico-SDK master calls in `I2C.c` with `hal_i2c_master_*`.
3. **Pico backend** `drivers/i2c_bus/hal_i2c_pico.c` (or per-port): thin 1:1
   wrappers over the exact SDK calls used today — **zero Pico behavior change**.
4. **ESP32 backend** `ports/esp32_s3/main/hal_i2c_esp32.c` over ESP-IDF
   `i2c_master` (reuse the bus plumbing in `esp32_freenove_i2c.c`).
5. Wire both into their ports' builds; keep `run_tests.sh` at 192/192.

### I²C slave — deferred, but kept compilable everywhere
The slave path in `I2C.c` is IRQ-driven and pokes RP2040 registers directly. To
avoid a target `#ifdef` in shared code, route the slave command paths through
`hal_i2c_slave_*` entry points: the **Pico backend** keeps its exact IRQ
behavior; the **ESP32 backend stubs** them (returns "not supported", `cmd_i2c`
surfaces the BASIC error). A real ESP-IDF slave impl is a later phase. This
keeps Phase 1 to the master path — the actual ESP32 need.

## 2. `hal/hal_pwm.h` — PWM contract (SERVO rides it)

Full contract, same rigor as I²C:

```c
int  hal_pwm_init(void);                       /* once, at boot */
/* Bind `channel` to `gpio` and start it at `freq_hz` / `duty_pct` (0–100).
 * For a paired second channel (the RP "B" channel / a second LEDC channel
 * sharing the timer) pass has_b/duty_b_pct. Returns 0 or negative errno. */
int  hal_pwm_configure(int channel, int gpio, float freq_hz,
                       float duty_pct, int has_b, float duty_b_pct,
                       int invert);
int  hal_pwm_set_duty(int channel, int which, float duty_pct); /* which=0/1 */
void hal_pwm_stop(int channel);
int  hal_pwm_channels(void);                   /* count, for bounds checks */
```

The contract is the *capability*; the **logic stays shared**. Today the PWM
math (frequency→wrap/clkdiv, duty scaling, the servo 50 Hz / position→duty
conversion) lives in the RP `vm_sys_pin.c` while ESP32 has stubs — i.e.
duplicated per port. The fix mirrors I²C: the PWM/servo logic calls
`hal_pwm_*`, and the hardware specifics drop into the backend.

### Refactor (makes the PWM path port-agnostic)
1. Add `hal/hal_pwm.h` (purity-clean).
2. `vm_sys_pwm_configure` / `vm_sys_pwm_set` / `vm_sys_pwm_off` /
   `vm_sys_servo_configure` call `hal_pwm_*` for the hardware, keeping the
   shared BASIC-facing semantics (`PWM ch,freq,duty[,duty2]`, `SERVO ch,pos`).
   `SERVO` = `hal_pwm_configure(ch, pin, 50.0, position→duty)`.
3. **Pico backend** `hal_pwm_pico.c` — 1:1 wrappers over the RP `pwm_set_*`
   slice calls `vm_sys_pin.c` makes today → zero Pico behavior change.
4. **ESP32 backend** `hal_pwm_esp32.c` — LEDC. 8 channels assignable to any
   GPIO; frequency per-timer (4 timers), so channels at the same frequency
   share a timer. (`esp32_backlight.c` already drives LEDC — reuse the setup.)
5. **ESP32 pin table**: give PWM-capable pins a channel id (drop the blanket
   `ESP32_NO_PWM`); `vm_sys_pin_setpin` accepts the PWM mode and binds
   pin → channel. Any pin → any free channel, set via `SETPIN`. Existing
   BASIC syntax unchanged.

### Channel model (the one decision)
LEDC's 8 channels are exposed as the `PWM` command's channels — any pin
assignable to any free channel, unlike RP's fixed slice↔pin. Channels sharing a
frequency share one of the 4 LEDC timers. This keeps `PWM ch,freq,duty` working
as-is.

## 3. SERVO

No separate driver — `vm_sys_servo_configure` is a thin layer over
`hal_pwm_configure` at 50 Hz, with the standard position→duty mapping
(`duty ≈ 5 + pos·0.05`, i.e. 1–2 ms on a 20 ms period). Falls out of the PWM
HAL for free on every port.

## 4. Board defaults (FREENOVE) — overridable

- Seed `Option.SYSTEM_I2C_SDA = GP16`, `SYSTEM_I2C_SCL = GP15` in the FREENOVE
  profile defaults (the board's single shared touch/audio I²C bus). `GENERIC`
  seeds nothing. The user can `SETPIN` any pins to override.
- No PWM/SERVO default pins needed — those are always user-assigned via `SETPIN`.

## Phasing

GPIO already rides `hal_pin.h` (the model). The two new HALs are **independent,
equal-weight deliverables** — they touch different shared files, so they can
land in either order or in parallel; one PR each keeps the Pico-regression
surface small.

- **PR — I²C master HAL**: `hal_i2c.h`, refactor `I2C.c`, Pico + ESP32 backends,
  slave stubs. Smoke `I2C OPEN/WRITE/READ` on the Freenove GP15/16 bus.
- **PR — PWM + SERVO HAL**: `hal_pwm.h`, the shared `vm_sys_pwm_*`/`servo`
  refactor, Pico + ESP32 (LEDC) backends, pin-table PWM caps. Smoke a PWM pin
  and a servo on the IO header.
- **Each PR**: build ESP32 **and** a Pico `.uf2` target (no regression),
  `run_tests.sh` 192/192, HAL purity gate green.
- **Later**: I²C slave ESP-IDF impl; `PORT`/`PULSE` if wanted.

## No-regression guarantee (Pico)

Refactoring `I2C.c` and the PWM syscalls touches Pico-linked code. The Pico
backends are 1:1 wrappers over the calls those files make **today**, so Pico
behavior is byte-identical. Gates: `run_tests.sh` stays 192/192, the HAL purity
gate passes, and at least one Pico `.uf2` target builds clean before merge.

## Open decisions

1. **I²C slave**: defer to a later phase (master-first), as above — confirm.
2. **PWM channel model**: expose LEDC's 8 channels as the `PWM` command's
   channels, any pin assignable via `SETPIN` — confirm.
3. **Pico backend placement**: `drivers/i2c_bus/hal_i2c_pico.c` (shared by all
   RP2 ports) vs per-port — lean shared-in-driver.
