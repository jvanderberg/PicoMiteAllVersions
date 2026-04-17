# Host HAL Plan

Collapse the host port into a clean hardware-abstraction layer so the host build re-uses the original MMBasic source (`Draw.c`, `FileIO.c`, `Audio.c`, `MM_Misc.c`, REPL) instead of re-implementing command handlers in `host/host_stubs_legacy.c`.

- **Branch:** `host-hal-refactor` (off `bridge-restoration`).
- **Predecessor plan:** [`bridge-restoration-plan.md`](bridge-restoration-plan.md). The bridge restoration landed the invariant that the interpreter is the primary runtime and the VM is a performance backend behind `FRUN`. This plan builds on that invariant — it does not revisit the VM/interpreter split.

## Invariants

1. **Device runtime path is identical after this work.** Source layout may change (code moved between files, new shared headers, `#ifdef MMBASIC_HOST` guards); the device firmware's instruction stream for any given BASIC line must match today. The gate is behavioral, not textual.
2. **The VM path is untouched.** `vm_sys_graphics_*_execute`, `vm_sys_file_*`, and the bytecode dispatch all stay as-is. Graphics/file commands have a native VM opcode; the VM never goes through `cmd_*`. This plan only reshapes the *interpreter* path on host.
3. **Host test suite stays green at every phase boundary.** No phase lands until `cd host/ && ./build.sh && ./run_tests.sh` passes in default (compare), `--interp`, and `--vm` modes.
4. **Device build stays green at every phase boundary.** `CMakeLists.txt` / `CMakeLists 2350.txt` must still build. Manual smoke-boot of RP2040 firmware after any phase that touches `Draw.c` / `FileIO.c`.

## Problem statement

The host Makefile excludes `Draw.c`, `FileIO.c`, `Audio.c`, `MM_Misc.c`, `GUI.c`, `PicoMite.c`, and all peripheral drivers from `CORE_SRCS`. To plug the gap, `host/host_stubs_legacy.c` (3,549 lines) defines 105 `cmd_*` and 37 `fun_*` entries. Roughly 90 of those are legitimate no-op stubs for hardware-only commands (`cmd_adc`, `cmd_i2c`, `cmd_pwm`, ...). The remainder are **divergent re-implementations** of interpreter command handlers:

| Area | Duplicated host entries | Shared equivalent |
|---|---|---|
| Graphics | `cmd_box`, `cmd_circle`, `cmd_line`, `cmd_pixel`, `cmd_text`, `cmd_triangle`, `cmd_polygon` | `Draw.c` (via `gfx_*_shared.c`) |
| File I/O | `cmd_load`, `cmd_save`, `cmd_open`, `cmd_copy`, `cmd_seek`, `FileLoadProgram` | `FileIO.c` |
| Audio | `cmd_play` | `Audio.c` |

These duplicates skip the vector/array paths, differ in error-reporting, and must be hand-mirrored every time the shared version changes. They exist purely because the core files won't compile on host today.

The VM is **not** the reason. `cmd_box` is never reached from VM dispatch — `OP_BOX` → `vm_sys_graphics_box_execute` → `DrawBox`, bypassing `cmd_*` entirely.

## Target architecture

```
┌─────────────────────────────────────────────────────────────┐
│                   Shared core (unchanged)                   │
│  MMBasic.c, Commands.c, Functions.c, Operators.c, …         │
│  MMBasic_REPL.c, MMBasic_Prompt.c, Editor.c                 │
│  bc_source.c, bc_vm.c, bc_runtime.c, vm_sys_*.c             │
│  gfx_*_shared.c                                             │
├─────────────────────────────────────────────────────────────┤
│     Shared interpreter commands (compile on both)           │
│  Draw.c  │ FileIO.c │ Audio.c │ MM_Misc.c  (portions)       │
│     — hardware touchpoints gated by #ifdef MMBASIC_HOST     │
│     — gated branches call into HAL                          │
├─────────────────────────────────────────────────────────────┤
│                        HAL surface                          │
│  host_fb_hal.h     — pixel plane, clear, refresh            │
│  host_fs_hal.h     — flash backing, drive switching         │
│  host_input_hal.h  — key polling                            │
│  host_console_hal.h— char I/O, banner                       │
│  host_timer_hal.h  — monotonic time, sleep                  │
│  host_audio_hal.h  — sound                                  │
│  host_sim_hal.h    — WS emit (--sim only)                   │
├─────────────────────────────────────────────────────────────┤
│             Host implementation of HAL                      │
│  host/host_fb.c, host/host_fs.c, host/host_audio.c, …       │
│  host/host_noop_stubs.c  — true no-op commands only         │
└─────────────────────────────────────────────────────────────┘
```

Each HAL header declares a small, abstract interface. Host provides a backing `.c`. Device provides a backing `.c` (or inlines the same calls as today — see Phase 2). Nothing in the shared source knows whether it is running on device or host.

## How the `#ifdef` contract works

A shared file that currently calls device-only code:

```c
// Draw.c — before
mutex_enter_blocking(&frameBufferMutex);
dma_channel_transfer_from_buffer_now(...);
mutex_exit(&frameBufferMutex);
```

Becomes:

```c
// Draw.c — after
#ifdef MMBASIC_HOST
    /* host build: no DMA, direct write to pixel plane */
    host_fb_begin();
    host_fb_blit_region(...);
    host_fb_end();
#else
    mutex_enter_blocking(&frameBufferMutex);
    dma_channel_transfer_from_buffer_now(...);
    mutex_exit(&frameBufferMutex);
#endif
```

- The `#else` branch is the **exact current device code**. No behavior change.
- If the same pattern recurs, promote it to an inline in a HAL header so both branches shrink.
- Device-only helpers referenced only from `#else` branches are allowed to remain device-only (no host stub needed).

Where possible, prefer the HAL wrap in a single place (one `host_fb_commit()` call wrapping DMA + mutex) rather than scattering `#ifdef`s through every function. This keeps the device source readable.

## Phased plan

Each phase ends with a green host build, green device build, and a commit. No phase depends on the next.

### Phase 0 — Setup & discovery (no code changes)

- Land this plan document.
- Grep-inventory the hardware touchpoints in each target file:
  - `Draw.c` — 73 hits on `frameBufferMutex|mutex_enter|mutex_exit|dma_channel|...`
  - `FileIO.c` — 1 hit (frameBufferMutex extern at line 58)
  - `MM_Misc.c` — 18 hits
  - `Audio.c` — 0 direct peripheral hits (Audio uses DMA indirectly through helpers)
- Produce a dependency map: each shared file → list of device-only symbols it references.
- Decide per-symbol: (a) stub in `host/hardware/*.h` (done already for most pico-sdk headers), (b) HAL call, (c) leave behind `#ifdef`.

**Exit gate:** dependency map lives in this doc, updated after survey.

#### Phase 0 dependency map (2026-04-17 survey)

Host stubs already defined in `host_stubs_legacy.c` for every `cmd_*`/`fun_*` that lives in the four target files — no "add a new stub" needed, only replace/delete as each file gets linked. `host/hardware/` already shims the pico-sdk headers referenced below (`flash.h`, `mutex.h`, `sync.h`, `time.h`, `multicore.h`, etc.).

**Draw.c (9,721 lines).** Hardware touchpoints cluster in three families:
- `multicore_fifo_push_blocking` — 23 calls across 8 functions (rp2350 dual-core LCD path; ClearScreen, set/closeframebuffer, bc_fastgfx_swap, merge/mergecolor, fun_mmcharheight, cmd_refresh).
- `mutex_enter_blocking(&frameBufferMutex)` / `mutex_exit` — 4 pairs, merge/mergecolor/blit region (wrap `host_fb_begin/end`).
- `__not_in_flash_func` — 6 macros on color conversion + fastgfx_swap_core1.
- Preprocessor: `PICOMITEVGA` (35+ blocks — VGA/3D/HDMI; leave device-only), `PICOMITEWEB` (2), `rp2350` (5+), `PICOMITE` (gates the multicore/mutex blocks).
- Whole-function `#ifndef MMBASIC_HOST` candidates: `fastgfx_swap_core1`, multicore-driven variants of `ClearScreen`/`setframebuffer`/`closeframebuffer`, `fun_getscanline` (VGA).
- Mostly portable (per-block gating only): `cmd_box`, `cmd_circle`, `cmd_line`, `cmd_polygon`, `cmd_pixel`, `cmd_triangle`, `cmd_blitmemory`, `cmd_sprite`, `merge`/`mergecolor` (after mutex wrap), `fun_rgb`, `fun_mmhres`/`fun_mmvres`, `fun_mmcharwidth`/`fun_mmcharheight`.

**FileIO.c (5,606 lines).** Almost entirely portable — device touch concentrated in flash-backed LittleFS:
- `flash_range_program` (13), `flash_range_erase` (14), `fs_flash_{read,prog,erase,sync}` (4) — all already stubbed in `host/hardware/flash.h`.
- `disable_interrupts_pico` / `enable_interrupts_pico` — defined inside FileIO.c (lines 311-327); wrap the whole pair in `#ifndef MMBASIC_HOST` (host: empty macros).
- `frameBufferMutex` extern at line 58 — `#ifdef PICOMITE` gate.
- `rp2350` mmap/psmap page-table blocks (5+) — device-only.
- Per-handler: `cmd_open`, `cmd_close`, `cmd_seek`, `cmd_flush`, `fun_loc`/`lof`/`eof`/`inputstr`, `cmd_chdir`, `fun_cwd`, `fun_dir` all portable as-is; `cmd_autosave` has a dual path (flash save + file — gate flash branch); `cmd_LoadImage` / `cmd_LoadJPGImage` need device display output wrapped.

**Audio.c (2,177 lines).** Hardware-bound — minimal portable surface. `cmd_play` (1037-1900) is the only command and it depends end-to-end on PWM (21 calls) / PIO (1) / flash. Strategy: HAL wraps `StartAudio`/`StopAudio`/DMA-buffer fill; on host, route to existing `host_sim_audio.c` PCM API. `iconvert`/`i2sconvert` are `__not_in_flash_func` but pure compute — portable.

**MM_Misc.c (6,474 lines).** Mix of portable + hardware-bound. Portable: `cmd_sort`, `cmd_longString` + friends (`fun_LGetStr`, `fun_LGetByte`, etc.), `fun_format`, date/time (`cmd_date`/`fun_date`/`cmd_time`/`fun_time`/`fun_day`/`fun_datetime`/`fun_epoch` — all once `time_us_64()` is HAL-routed), `cmd_pause` (busy loop → host sleep), `cmd_poke`/`fun_peek` (host: restricted address range). Device-only: `cmd_settick`, `cmd_watchdog`, `fun_restart`, `cmd_csubinterrupt`, `cmd_ireturn`, INT1-INT4 GPIO setup (`gpio_set_irq_enabled` ×8 at 4602-4619), `cmd_cpu`, `fun_device`. Preprocessor: `PICOMITEVGA` (VGA recovery), `rp2350` (PRNG/mmap), `PICOCALC` (I2C keyboard), `USBKEYBOARD`.

**Gating strategy** for the four files:
1. Flash ops: define `disable_interrupts_pico`/`enable_interrupts_pico` as `((void)0)` macros on host; real flash_range_* are already no-op stubs.
2. Multicore: `#ifdef PICOMITE` blocks in Draw.c already exist — extend with `#ifndef MMBASIC_HOST` where PICOMITE is defined on both (or rely on not defining PICOMITE when `MMBASIC_HOST` is set; verify current flag mix).
3. `time_us_64()`: route through existing host shim (already stubbed).
4. GPIO IRQ / PWM / PIO: wrap whole blocks / whole functions.
5. `PICOMITEVGA` / `PICOMITEWEB`: not defined on host → blocks drop out naturally.

Host stub line count impact (estimated from plan): Phase 2 ~1,200 lines deleted, Phase 3 ~300, Phase 4 ~75, Phase 5 ~200-400. Target final `host_noop_stubs.c` under 1,500 lines.

### Phase 1 — Pure moves out of `host_stubs_legacy.c`

No behavior change. Just untangle the stubs file.

- Move `host_load_key_script` + `--keys-after-ms` plumbing → `host_main.c` (test-harness feature, not HAL).
- Move `host_sim_tick_body` + WebSocket polling → `host_sim_server.c`.
- Extract pure HAL implementations into new files:
  - `host/host_fb.c` — framebuffer allocation, `host_put_pixel`, `host_fb_ensure`, pixel-dispatch setup.
  - `host/host_time.c` — `host_time_us_64`, `host_sync_msec_timer`.
- `host_stubs_legacy.c` → `host_noop_stubs.c` once the extractions are done. Keep only empty-body `cmd_*` / `fun_*` for hardware features the host genuinely doesn't support.

**Exit gate:** host build compiles, all tests pass, file is <2500 lines.

### Phase 2 — `Draw.c` compiles on host

The biggest phase. Brings graphics commands back to shared.

1. Introduce `host_fb_hal.h`:
   ```c
   void host_fb_init(int w, int h);
   void host_fb_put_pixel(int x, int y, uint32_t rgb);
   uint32_t host_fb_get_pixel(int x, int y);
   void host_fb_clear(uint32_t rgb);
   void host_fb_begin(void);    /* stand-in for mutex_enter_blocking */
   void host_fb_end(void);      /* stand-in for mutex_exit */
   void host_fb_refresh(void);  /* stand-in for DMA blit */
   ```
2. In `Draw.c`, gate every device-only block:
   - `frameBufferMutex` → `host_fb_begin/end` on host.
   - DMA pixel pushes → `host_fb_refresh()` on host.
   - Display-mode specialisations (`PICOMITEVGA`, `PICOMITEWEB`) stay device-only; host uses the generic path.
3. Provide `DrawBox`, `DrawLine`, `DrawPixel`, `DrawCircle`, `DrawTriangle`, `DrawChar` on both builds. The host versions currently in `host_stubs_legacy.c:3354+` become the backing impls for the `MMBASIC_HOST` branches — they already call `host_put_pixel`, which is what `host_fb_put_pixel` wraps.
4. Add `Draw.c` to `CORE_SRCS` in `host/Makefile`.
5. **Delete** from `host_noop_stubs.c`: `cmd_box`, `cmd_circle`, `cmd_line`, `cmd_pixel`, `cmd_text`, `cmd_triangle`, `cmd_polygon`, and the `host_fill_polygon_edges` / `host_draw_triangle_pixels` / `host_draw_text` / `host_draw_char` / `host_glyph_rows` helpers that implemented them.

**Expected line savings:** ~1,200 lines from `host_noop_stubs.c`.

**Risks:** `Draw.c` has 73 hardware touchpoints. Some may be in seldom-used display modes (e.g. PICOMITEVGA dual-core) that the host should not try to support — wrap the whole function in `#ifndef MMBASIC_HOST` and leave the corresponding `cmd_*` as a stub on host. Document which commands are host-unsupported.

**Exit gate:**
- `cd host/ && ./build.sh && ./run_tests.sh` all-green.
- Device firmware boots, runs `demo_plasma.bas` or equivalent visual test.
- Pixel-perfect comparison: run a graphics program through interpreter and VM on host, diff framebuffer snapshots — should match byte-for-byte.

### Phase 3 — `FileIO.c` compiles on host

Smaller scope than Phase 2. Most of `FileIO.c` is already portable (FatFS lives in `ff.c`, already linked on host via `vm_host_fat.c`).

1. Introduce `host_fs_hal.h`:
   ```c
   int host_fs_drive_select(const char *drive);    /* A: vs B: switching */
   int host_fs_path_resolve(const char *in, char *out, int max);
   void host_fs_flash_backing(void);               /* host "flash" = RAM buffer */
   ```
2. Gate the device-specific drive-switching and flash-program paths in `FileIO.c` behind `#ifdef MMBASIC_HOST`.
3. Handle `frameBufferMutex` reference at `FileIO.c:58` — this is a stray extern used during LOAD to pause display refresh. Gate it.
4. Add `FileIO.c` to `CORE_SRCS`.
5. **Delete** from `host_noop_stubs.c`: `cmd_load`, `cmd_save`, `cmd_open`, `cmd_copy`, `cmd_name`, `cmd_seek`, `FileLoadProgram`.

**Expected line savings:** ~300 lines.

**Exit gate:** same validation as Phase 2, plus test harness `LOAD` / `SAVE` / `RUN "file"` round-trip passes on host.

### Phase 4 — `Audio.c` compiles on host

`Audio.c` has zero direct peripheral calls by grep — the hardware interaction is through helper functions (`StartAudio`, `StopAudio`, etc.) defined elsewhere.

1. Introduce `host_audio_hal.h` with the existing host audio API (`host_sim_audio_play`, etc.) as the HAL.
2. Stub the helper functions (`StartAudio`, `StopAudio`, DMA audio buffer setup) via the HAL.
3. Add `Audio.c` to `CORE_SRCS`.
4. **Delete** `cmd_play` from `host_noop_stubs.c` (75 lines).

**Exit gate:** `play_tone.bas` test runs, `--sim` mode plays audio to browser.

### Phase 5 — `MM_Misc.c` (partial)

Many commands here are fundamentally hardware-bound (watchdog, RTC, TIMER ticks). Only bring over commands that have reasonable host equivalents — `cmd_pause`, `cmd_timer`, timer interrupt scheduling. Leave watchdog / RTC / CPU-clock commands as no-op stubs in `host_noop_stubs.c`.

**Exit gate:** `cmd_pause` / `cmd_timer` / `cmd_settick` tests pass on host with shared source.

### Phase 6 — REPL & banner unification

`host_main.c:517-525` emits a divergent banner. `PicoMite.c:3793` uses the shared `MMBasic_RunPromptLoop`. Extract the banner into `MMBasic_REPL.c` as `MMBasic_PrintBanner(void)`, call from both entry points.

Minor — maybe 30 lines — but closes the last REPL-shaped gap.

**Exit gate:** banner matches device, `--sim` + raw TTY + piped modes all still start correctly.

### Phase 7 — Cleanup & rename

- Rename `host_stubs_legacy.c` → `host_noop_stubs.c` (final name).
- Split further if warranted: `host_gfx_hal.c`, `host_fs_hal.c`, `host_peripheral_stubs.c`.
- Remove the `(void)` unused-parameter suppressions that no longer apply.
- Update `host/README.md` with the HAL architecture.
- Update `CLAUDE.md` memory: Host build is no longer "its own MMBasic port" — it's a HAL target using the shared interpreter source.

**Exit gate:** no file over 1000 lines in `host/`, README current, one commit.

## What does *not* change

- `vm_sys_graphics.c`, `vm_sys_file.c`, `vm_sys_audio.c`, `vm_sys_time.c`, `vm_sys_pin.c`, `vm_sys_input.c` — VM syscalls stay as-is. The VM dispatch path is completely untouched.
- `bc_vm.c`, `bc_source.c`, `bc_runtime.c`, `bc_alloc.c` — bytecode compiler/VM stay as-is.
- `gfx_*_shared.c` — already shared, no change.
- `MMBasic.c`, `MMBasic_REPL.c`, `MMBasic_Prompt.c`, `Editor.c`, `Commands.c`, `Functions.c` — already shared, stay shared.
- Device peripheral drivers (`SPI.c`, `I2C.c`, `Keyboard.c`, display drivers) — remain device-only, not linked on host.
- The "no `VM-only device` build" constraint from the bridge plan — still holds.

## Validation gates (applied after every phase)

1. **Host build:** `cd host/ && ./build.sh` — zero warnings, zero errors.
2. **Host tests, default compare:** `cd host/ && ./run_tests.sh` — all PASS.
3. **Host tests, interp only:** `./run_tests.sh --interp`.
4. **Host tests, VM only:** `./run_tests.sh --vm`.
5. **Device build:** `cmake` RP2040 and RP2350 targets build clean.
6. **Device smoke boot:** firmware flashed to RP2040 boots to prompt and runs a demo `.bas`. Required after Phase 2 (Draw.c) and Phase 3 (FileIO.c) specifically.
7. **Framebuffer diff test** (new, Phase 2 forward): run a graphics program through interpreter + VM, snapshot `host_fb_get_pixel` over the frame, assert byte-equal. Catches divergences between the shared `Draw.c` path and the VM's direct path.

## Open questions

- **Does `PICOMITEVGA` dual-core rendering need a host story?** Probably not — host renders single-threaded. Leave `#ifdef PICOMITEVGA` blocks as device-only; the corresponding commands remain host no-ops. Decide in Phase 2 discovery.
- **What about `Custom.c`, `GUI.c`?** Out of scope for now. They have zero commands shared between host and device currently, so there's no duplication to unwind. Revisit after Phase 7 if we want full parity.
- **Does the VM need its own host HAL entry point?** The VM already uses `host_put_pixel` indirectly via `DrawBox` / `DrawLine`. Once those route through the `host_fb_hal.h` API, the VM also benefits — no additional work.

## Rollback

Every phase is a single commit (or small commit chain) on the `host-hal-refactor` branch. If a phase breaks a downstream integration, revert the phase commit and re-attempt. The HAL headers added in earlier phases can remain (they're additive). If the whole effort stalls, the branch can be abandoned and `bridge-restoration` is unaffected.

## Ordering rationale

Phase 2 (Draw.c) is the largest and highest-value phase — it removes the most duplication and closes the most drift risk. But it's also the one most likely to surface surprises in the pico-sdk dependency graph. If Phase 2 reveals that `Draw.c` is more entangled than expected, bail and narrow to the three smallest commands (cmd_box, cmd_pixel, cmd_line) as a proof-of-concept before committing to the full file.

Phase 3 and 4 can be parallelized if desired (independent files). Phase 5 is optional — it's a smaller win and `MM_Misc.c` has more fundamentally device-bound code than the others.
