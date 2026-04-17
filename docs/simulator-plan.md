# MMBasic Simulator Plan

## Status (as of this session)

- **Phase 0 — host REPL on terminal: ✅ DONE.** Interactive MMBasic over a real TTY, using the device's `EditInputLine` (history, arrow keys, F-keys, cursor editing, EDIT). LOAD / SAVE / FILES / RUN / FRUN work against a real host directory (`--sd-root` flag, default cwd). 191/191 existing tests still pass. Commits: `51bb8ce` (initial host REPL + monolith split), `da6d070` (REPL state-bug fixes).
- **Phase 1 — web shell: 🚧 next up.** Everything below under "Phase 1" is what to build next.

See [References](#references) for where things live.

## Goal

Browser-based desktop simulator for the PicoCalc/PicoMite. Same interpreter and VM as the device — only the I/O backend changes. The host binary embeds an HTTP + WebSocket server; a React frontend in the browser is the display, keyboard, and audio output.

Design principles:
- The interpreter and VM stay untouched. Even the device's main interactive loop stays where it is — we only redirect its inputs and outputs.
- **Prefer wrapping over modifying existing MMBasic sources.** New behavior lives in `host/` as thin shims, hooks, and function overrides. Touch `MMBasic.c`, `PicoMite.c`, `Commands.c`, etc. only when there is no reasonable wrapper alternative, and keep any change minimal and local.
- The framebuffer is the source of truth. Pixels stream out, key events stream in, audio is sent as commands.
- The web frontend is a thin shell: receive pixels, draw to canvas, send keys back. No business logic in the browser.

## Architecture

```
┌──────────────────────────┐    WebSocket     ┌──────────────────────────┐
│  mmbasic_sim (C)         │ ◄─────────────►  │  Browser (React)         │
│  ┌────────────────────┐  │                  │  ┌────────────────────┐  │
│  │ MMBasic interpreter│  │  binary frames → │  │ <Display/> canvas  │  │
│  │   + bytecode VM    │  │  ← key events    │  │ <Keyboard/>        │  │
│  └────────────────────┘  │  audio cmds  →   │  │ <AudioEngine/>     │  │
│  host_stubs (existing)   │                  │  │   (WebAudio)       │  │
│  ┌────────────────────┐  │                  │  └────────────────────┘  │
│  │ host_sim_server.c  │  │  HTTP: index.html, JS bundle, assets        │
│  │ (Mongoose)         │  │                  │                          │
│  └────────────────────┘  │                  │                          │
└──────────────────────────┘                  └──────────────────────────┘
```

The REPL renders into the framebuffer using the existing on-screen text renderer — no separate text panel. The browser is intentionally dumb: receive pixels, draw to canvas, send keys back.

## Decisions

| Decision | Choice | Rationale |
|---|---|---|
| HTTP + WS library | Mongoose (vendored, single .c/.h) | Designed for embedding in C apps; WS built in; battle-tested |
| Frontend | React + Vite + TypeScript | Standard, easy multi-pane UI, good DX |
| Framebuffer wire format | Raw RGBA 320×240×4 = 307 KB/frame | At 60fps = ~18 MB/s, trivial on loopback. Optimize later only if needed |
| Audio | Commands → WebAudio | Avoids PCM streaming and underrun handling; matches MMBasic's tone-centric audio model |
| REPL UI | Rendered into framebuffer | PicoCalc-faithful; reuses device's text-on-screen renderer; one source of truth |
| Bind address | `127.0.0.1` by default | Safe default; `--listen 0.0.0.0` to share |
| Auto-open browser | Yes, on `--sim` start | Better DX |
| Frontend asset serving | `web/dist/` from disk in dev | Phase 5: `xxd -i` bundle into binary for single-file deploy |
| Build target | Separate `mmbasic_sim` binary, new `make sim` | Existing CI / test harness untouched |
| Terminal REPL | Permanent feature, not just scaffolding | Useful for CI, scripting, SSH; can coexist with `--sim` for incremental testing |

## Phases

### Phase 0 — Host REPL on terminal ✅ DONE

Delivered:

- `./host/mmbasic_test --repl [--sd-root DIR]` enters an interactive MMBasic session. Default `sd-root` is cwd.
- Uses the device's real `EditInputLine` (moved to `MMBasic_Prompt.c` — see below). UP/DOWN history, LEFT/RIGHT cursor edit, HOME/END, INSERT, F1–F12, TAB, backspace — all working on a real terminal via VT100 escape codes.
- Terminal raw mode and escape-sequence decoding in `host/host_terminal.{c,h}`. Terminal size auto-detected via `TIOCGWINSZ`. Cooked-mode fallback when stdin is piped (CI / scripts still work).
- `EDIT` runs over VT100 and calls through to the full device editor. F1 saves edits back to ProgMemory.
- `LOAD "foo.bas"`, `SAVE "foo.bas"`, `FILES`, `RUN "foo.bas"`, `FRUN "foo.bas"` route through the host filesystem at `host_sd_root`. `cmd_save` detokenises via `llist()`. `cmd_files` uses POSIX `opendir`/`readdir` in `host/host_fs.{c,h}` (isolated because POSIX `DIR` collides with FatFS `DIR`).
- `MEMORY`, `NEW`, error recovery all work. Ctrl-D at the prompt exits cleanly (gated on `editactive == 0` so it's still cursor-right inside EDIT).
- Simulated flash (`flash_range_erase`, `flash_range_program`, `FlashWriteByte`-family stubs) backs writes to `flash_prog_buf` so NEW / SAVE / EDIT-save actually persist. `load_basic_source` erases ProgMemory before writing, matching device behavior.
- Test harness (`mmbasic_test program.bas`) unchanged. All 191 tests still pass.

**PicoMite.c monolith split** (checkpointed in this phase because the host REPL wanted to reuse these):

| New file | Contents | Notes |
|---|---|---|
| `MMBasic_Prompt.c` | `EditInputLine`, `InsertLastcmd`, `lastcmd[]`, `MMPromptPos` | Host + device share the same line editor |
| `MMBasic_Print.c` | `PRet`/`PInt`/`PFlt`/`SRet`/`SInt`/`SIntComma`/… (12 print formatters) | Deleted 11 duplicate host stubs |
| `MMBasic_REPL.c` | `MMBasic_RunPromptLoop()`, `commandtbl_decode`, `transform_star_command` | Host's `run_repl()` calls the same loop device's `main()` calls |

PicoMite.c: 5065 → 4131 lines (−934). Both `CMakeLists.txt` and `CMakeLists 2350.txt` updated.

**Deferred from Phase 0 (known limitations):**

- `PLAY MODFILE` / `PLAY WAV` — no audio at all on host yet. Phase 3.
- `OPTION LIST` (prints nothing on host), `AUTO`, and assorted device-specific commands remain no-op stubs in `host_stubs_legacy.c`.
- Graphics commands (`BOX`/`LINE`/`CIRCLE`/`PIXEL`/`POLYGON`/`TEXT`) use simplified scalar-only bespoke host implementations; device uses `gfx_*_shared.c` with array-form support. Phase 4 polish: wire host into the shared path with host draw callbacks.
- HDMI/VGA display pipeline (`QVga*`, `HDMIloop0-3`, `HDMICore`, `dma_irq_handler0`) is still in `PicoMite.c`. Device-only; low priority to extract since host will never touch it.

### Phase 1 — HTTP + WS server, framebuffer streaming 🚧 NEXT

1. **Vendor Mongoose** at `host/vendor/mongoose.{c,h}` — single-file embeddable HTTP + WS server.
2. **Scaffold `web/`** — Vite + React + TypeScript. Single `<Display/>` component that opens a WebSocket to `ws://<host>/ws`, receives binary RGBA frames, and draws each into a 320×240 `<canvas>` via `putImageData`. CSS `image-rendering: pixelated` + 2-3× scale.
3. **New `host/host_sim_server.c`** — Mongoose event loop. HTTP serves `web/dist/` static files. WS endpoint at `/ws` handles connections and pushes frames.
4. **Framebuffer-flip hook** in `host_stubs_legacy.c` — when `host_framebuffer` content changes (or on a 60Hz timer), call into the server to broadcast a binary WS message. The host framebuffer already exists as `uint32_t host_framebuffer[]` — it's what the test harness captures pixels from. No restructure needed, just a hook.
5. **New `--sim [--port N] [--listen ADDR]` flag** in `host_main.c`. `--sim` implies `--repl` behavior (interactive session, not one-shot); pairing with `--repl` means terminal I/O + web display coexist — a useful intermediate state for testing before keyboard-over-WS exists.
6. **Build target** — new `make sim` produces `mmbasic_sim` binary. `make` / `make test` stay on `mmbasic_test`. CI untouched.

**Acceptance:** `./mmbasic_sim --sim sample.bas` opens browser, shows running graphics at up to 60fps. Can open multiple browsers (one broadcasts-to-all is fine — multi-client support is out-of-scope, last connection wins or server broadcasts to all; TBD during implementation).

**Size check:** 320×240×4 = 307 KB/frame × 60 fps = ~18 MB/s. Loopback handles that trivially. If we ever care about WAN performance, switch to RGB565 (halves it) or dirty-rect deltas — but not in v1.

### Phase 2 — Keyboard input

- `<Keyboard/>` React component captures `keydown` on window, translates to PicoCalc keycodes, sends `{op:"key", code: N}` (JSON) over WS.
- Map PicoCalc-specific keys (Fn modifier, special keys) to browser equivalents. Read keyboard driver source for exact keycode table (probably `i2c_keyboard.c` or similar); produce a shared `picocalc_keymap.{h,ts}`.
- Server feeds events into the existing `host_keydown()` / `host_key_script` path. `INKEY$`, `KEYDOWN`, `INPUT` and the REPL's `EditInputLine` all work for free.
- Acceptance: REPL fully usable in the browser. EDIT works.

### Phase 3 — Audio commands

- New `host/host_sim_audio.c` replaces the no-op stubs in `vm_sys_audio.c` for host.
- Each `vm_sys_audio_*` syscall translates to a JSON message:
  - `{op:"tone", left_hz, right_hz, ms?}` — start tones (with optional duration)
  - `{op:"stop"}` — silence
  - `{op:"sound", channel, freq, wave, vol}` — `PLAY SOUND`
- `<AudioEngine/>` keeps a pool of `OscillatorNode`s per channel, schedules with WebAudio's precise timing.
- Defer `PLAY MODFILE` / `PLAY WAV` to v2 (needs file streaming, not commands).

### Phase 4 — Polish

- Auto-open browser on `--sim` start (`open` on macOS, `xdg-open` on Linux, `start` on Windows).
- Window title shows running program / interpreter status.
- Configurable frame rate cap (`--fps N`).
- Status indicator in UI: connected, running, error.
- File upload: drag `.bas` file onto the page, server saves it to `host_sd_root`.
- Wire graphics commands into `gfx_*_shared.c` so host supports array-form BOX/LINE/CIRCLE/etc. (removes ~400 lines of bespoke scalar-only host code).

### Phase 5 — Optional extras

- GPIO panel: side panel showing `host_pin_value[]` and `host_pin_mode[]` state. Could be interactive (click a pin to drive it).
- Single-binary deploy: `xxd -i` the `web/dist/` bundle into a C header, embed in binary. `mmbasic_sim` becomes a single self-contained executable.
- Serial console panel: separate `<Console/>` that mirrors anything written to the simulated USB CDC. Useful for serial-port-style interaction without disturbing the main display.
- `PLAY MODFILE` / `PLAY WAV` via PCM streaming (binary WS frames + AudioWorklet).
- HDMI/VGA display pipeline extraction from `PicoMite.c` (only if there's appetite for larger device testing).

## Wire format

**Server → client:**
- Binary message, 307,200 bytes: framebuffer RGBA, row-major. Always full frame in v1.
- JSON message: `{op:"tone"|"stop"|"sound"|...}` for audio commands and status updates.

**Client → server:**
- JSON message: `{op:"key", code: N, down: bool}` for key events.
- JSON message: `{op:"upload", name: "...", data: "<base64>"}` (phase 4).

A shared `web/src/protocol.ts` defines message types; the C side uses ad-hoc encoding (small enough that a header isn't worth it).

## Build integration

- New target `make sim` (or `./host/build_sim.sh`) builds `mmbasic_sim` separately from `mmbasic_test`. Existing `host/build.sh` and `host/run_tests.sh` untouched.
- Mongoose and the React build add no dependencies to the test harness.
- CI continues to build and run only `mmbasic_test`.

## Keymap

PicoCalc keys are mostly ASCII. Function keys, Fn-modifier combinations, and special keys will be mapped on demand by reading the device's keyboard driver source — not pre-investigated. Phase 2 will produce a small `picocalc_keymap.{h,ts}` shared between server and client.

## File layout (after Phase 1)

```
host/
  host_main.c                # --sim / --repl / --sd-root / --listen / --port
  host_sim_server.c          # NEW in Phase 1: Mongoose HTTP+WS, frame push, event dispatch
  host_sim_audio.c           # NEW in Phase 3: vm_sys_audio_* → WS audio commands
  host_stubs_legacy.c        # add framebuffer-flip hook, key inject hook
  host_terminal.{c,h}        # DONE: termios raw mode, escape decoding
  host_fs.{c,h}              # DONE: POSIX directory listing for FILES/LOAD/SAVE
  vendor/mongoose.{c,h}      # NEW in Phase 1: vendored, MIT
  Makefile                   # add `sim` target
web/
  package.json
  vite.config.ts
  tsconfig.json
  index.html
  src/
    main.tsx
    App.tsx
    Display.tsx              # <canvas> + WS frame handler
    Keyboard.tsx             # window keydown listener → WS
    AudioEngine.tsx          # WebAudio command processor
    protocol.ts              # message type definitions
    ws.ts                    # WS client wrapper
MMBasic_Prompt.c             # DONE: EditInputLine + lastcmd + MMPromptPos
MMBasic_Print.c              # DONE: PRet/PInt/PFlt/...
MMBasic_REPL.c               # DONE: MMBasic_RunPromptLoop() + helpers
docs/
  simulator-plan.md          # this doc
```

## Out of scope (for v1)

- I2C / SPI peripheral emulation (RTC, expansion boards)
- USB host emulation
- Real-time GPIO interaction (PWM frequency monitoring, etc.) — basic value display only
- Multi-client support (one browser at a time; second connection replaces first or broadcasts)
- Save state / snapshots
- Network sharing beyond `--listen 0.0.0.0` (no auth, no TLS)

## Phase 0 findings (read before Phase 1)

Notes from building Phase 0 that a fresh conversation should know before touching Phase 1:

- **`host_sd_root`** is the REPL's filesystem root, defined in `host_stubs_legacy.c`. `NULL` in the test harness (falls through to in-memory FatFS). Non-NULL in `--repl` mode (cwd by default, overridden by `--sd-root`). Phase 1's server should inherit this — serving `web/dist/` is a separate concern but the file-upload path in Phase 4 writes into `host_sd_root`.
- **Framebuffer is `host_framebuffer`** — uint32_t RGBA buffer, allocated in `host_stubs_legacy.c`. Drawing primitives (`host_fill_rect_pixels`, `host_draw_line_pixels`, `host_draw_triangle_pixels`, `host_draw_char`, `host_draw_text`) write directly. No existing "flip" event; Phase 1 must introduce one. Simplest: a dirty flag + a 60Hz timer, or hook every framebuffer-writing primitive.
- **Text rendering already goes through the framebuffer.** `MMPrintString` → host stdout, but device-style graphics PRINT and `TEXT` command draw into `host_framebuffer` via `host_draw_text`. So as soon as the canvas is hooked up, text-mode programs show correctly *and* the REPL text display (if `Option.DISPLAY_CONSOLE` is enabled) would appear.
- **`Option.DISPLAY_CONSOLE`** is currently 0 in the REPL (console text goes to terminal, not framebuffer). Phase 1 should decide: keep as-is (browser shows only graphics, terminal shows text), or set to 1 (browser shows a PicoCalc-faithful combined console+graphics). The plan says "framebuffer is source of truth" — argues for setting it to 1 in `--sim` mode.
- **Piped-input mode** is handled separately from raw-mode in `host/host_terminal.c`. Phase 1's server is a third input source. Keep them orthogonal — the server path shouldn't go through `host_terminal.c` at all.
- **Simulated flash is 256 KB** (`flash_prog_buf`); first half is program area, second half is CFunction/Font area (0xFF-filled). `load_basic_source` now erases the program area before writing; Phase 1 doesn't need to worry about this.
- **`editactive` flag** is defined in `Editor.c` only under `#ifdef PICOMITEVGA`; host redefines it in `host_stubs_legacy.c`. Used by the REPL to gate Ctrl-D-exit. Phase 1's server may want to check it too if it intercepts keys.
- **`.claude/` and `build_rp2040/`** are gitignored-adjacent (the former is this session's local state, the latter is a build artifact). Don't commit either.

## Risks and unknowns (for Phase 1+)

- **Framebuffer flip granularity.** Currently every pixel-level primitive writes directly; there's no "present" step. Naive approach: 60Hz timer broadcasts the buffer regardless of changes. Better: dirty flag set by each primitive, cleared on broadcast. Best: track dirty rectangles — but raw full-frame at 18 MB/s on loopback is plenty fine.
- **Mongoose event loop vs main thread.** Mongoose is single-threaded event-loop based; so is the REPL (blocks on stdin). Phase 1 needs to decide: spawn Mongoose on a background thread (then framebuffer hook must be thread-safe), or reverse — put MMBasic interpreter on a background thread and run Mongoose on main. A background Mongoose thread is simpler for MVP.
- **EDIT in a browser.** Full-screen editor with arrow keys, page up/down — the browser will want to scroll/select instead. Need `preventDefault()` on captured keys, possibly a "capture keyboard" toggle.
- **Audio timing.** WebAudio scheduling is sample-accurate but the WS hop adds jitter. Fine for tones; non-issue once we accept it isn't a music engine.
- **Auto-open browser.** On macOS `open http://localhost:PORT` works. Make sure the HTTP server is listening before opening, or the browser gets "connection refused" and the user has to refresh.

## References

- **Commits**:
  - `51bb8ce` — Add host REPL and begin breaking up PicoMite.c
  - `da6d070` — Fix REPL state bugs: load stale tail, F1 save, Ctrl-D, inpbuf echo
- **Existing host build**: `host/host_main.c`, `host/host_stubs_legacy.c`, `host/Makefile`, `host/build.sh`
- **VM syscall ABI**: `vm_sys_*.h`, `bytecode.h`
- **Framebuffer primitives**: `host_stubs_legacy.c` (search for `host_fill_rect_pixels`, `host_draw_*`)
- **Mongoose**: https://github.com/cesanta/mongoose
