# MMBasic Simulator Plan

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

### Phase 0 — Host REPL on terminal (no web)

Goal: interactive MMBasic on the terminal. Foundation for everything else, and a permanent feature — useful for CI, scripting, and SSH sessions where graphics aren't needed.

- Locate the device's main interactive loop (likely in `MMBasic.c` — read line, tokenize, execute, print prompt). **Do not move it.** Identify any device-only dependencies (USB CDC polling, hardware timers) and stub them in the host build.
- Add `--repl` flag to `host/host_main.c` that enters the loop instead of running a script. Reads stdin, writes stdout.
- Verify `RUN`, `LIST`, `NEW`, `FILES`, `AUTO`, `SAVE`, `LOAD` work on host.
- `EDIT` is the canonical full-screen test, but it needs the framebuffer + key events. Defer until phase 2 has those wired.
- Terminal REPL has no graphics. Programs that draw will silently no-op or crash (TBD — depends on whether host_framebuffer is currently allocated). Phase 1 fixes this by adding the web display.

### Phase 1 — HTTP + WS server, framebuffer streaming

- Vendor Mongoose (`host/vendor/mongoose.{c,h}`).
- New file `host/host_sim_server.c`:
  - HTTP serves `web/dist/` static files
  - WS endpoint at `/ws`
  - Background thread or poll-based event loop
- Hook into framebuffer flip in `host_stubs_legacy.c` — when the framebuffer changes, push a binary WS frame to all connected clients.
- New `--sim [--port N] [--listen ADDR]` flag in `host_main.c`.
- Minimal `web/` scaffold: Vite + React + TS. Single `<Display/>` component opens WS, draws incoming `ImageData` to a 320×240 `<canvas>` with `image-rendering: pixelated` and CSS scaling.
- Acceptance: `./mmbasic_sim --sim sample.bas` opens browser, shows running graphics at 60fps.
- **Useful intermediate state**: `--repl --sim` runs the terminal REPL (stdin/stdout for input/output) AND mirrors the framebuffer to the web client. Lets us verify graphics streaming end-to-end before keyboard-over-WS exists in phase 2.

### Phase 2 — Keyboard input

- `<Keyboard/>` component captures `keydown` on the window, translates to PicoCalc keycodes, sends `{op:"key", code: N}` JSON over WS.
- Map PicoCalc-specific keys (Fn modifier, special keys) to browser equivalents.
- Server feeds events into the existing `host_keydown()` / `host_key_script` path. `INKEY$`, `KEYDOWN`, `INPUT` all work for free.
- Acceptance: REPL is fully usable in the browser. EDIT works.

### Phase 3 — Audio commands

- New `host/host_sim_audio.c` replaces the no-op stubs in `vm_sys_audio.c` host build.
- Each `vm_sys_audio_*` syscall translates to a JSON message:
  - `{op:"tone", left_hz, right_hz, ms?}` — start tones (with optional duration)
  - `{op:"stop"}` — silence
  - `{op:"sound", channel, freq, wave, vol}` — PLAY SOUND
- `<AudioEngine/>` keeps a pool of `OscillatorNode`s per channel, schedules with WebAudio's precise timing.
- Defer `PLAY MODFILE` and `PLAY WAV` to v2 (those need file streaming, not commands).

### Phase 4 — Polish

- Auto-open browser on `--sim` start (use `open` on macOS, `xdg-open` on Linux, etc.)
- Window title shows running program / interpreter status
- Configurable frame rate cap (`--fps N`)
- Status indicator in UI: connected, running, error
- File upload: drag a `.bas` file onto the page, server saves it to the simulated SD card

### Phase 5 — Optional extras

- GPIO panel: side panel showing `host_pin_value[]` and `host_pin_mode[]` state. Could be interactive (click a pin to drive it).
- Single-binary deploy: `xxd -i` the `web/dist/` bundle into a C header, embed in binary. `mmbasic_sim` becomes a single self-contained executable.
- Serial console panel: separate `<Console/>` that mirrors anything written to the simulated USB CDC. Useful for serial-port-style interaction without disturbing the main display.
- PLAY MODFILE / WAV via PCM streaming (binary WS frames + AudioWorklet).

## Wire format

**Server → client:**
- Binary message, 307_200 bytes: framebuffer RGBA, row-major. Always full frame in v1.
- JSON message: `{op:"tone"|"stop"|"sound"|...}` for audio commands and status updates.

**Client → server:**
- JSON message: `{op:"key", code: N, down: bool}` for key events.
- JSON message: `{op:"upload", name: "...", data: "<base64>"}` (phase 4).

A shared `web/src/protocol.ts` defines message types; the C side uses ad-hoc encoding (small enough that a header isn't worth it).

## Build integration

- New target `make sim` (or `./host/build_sim.sh`) builds `mmbasic_sim` separately from `mmbasic_test`. Existing `host/build.sh` and `host/run_tests.sh` are untouched.
- Mongoose and the React build add no dependencies to the test harness.
- CI continues to build and run only `mmbasic_test`.

## Keymap

PicoCalc keys are mostly ASCII. Function keys, Fn-modifier combinations, and special keys will be mapped on demand by reading the device's keyboard driver source — not pre-investigated. Phase 2 will produce a small `picocalc_keymap.{h,ts}` shared between server and client.

## File layout

```
host/
  host_main.c                # adds --sim, --repl, --listen, --port flags
  host_sim_server.c          # NEW: Mongoose HTTP+WS, frame push, event dispatch
  host_sim_audio.c           # NEW: vm_sys_audio_* → WS audio commands
  host_stubs_legacy.c        # add framebuffer-flip hook, key inject hook
  vendor/mongoose.{c,h}      # vendored, MIT
  Makefile                   # add new objects, link pthread
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
docs/
  simulator-plan.md          # this doc
```

## Out of scope (for v1)

- I2C / SPI peripheral emulation (RTC, expansion boards)
- USB host emulation
- Real-time GPIO interaction (PWM frequency monitoring, etc.) — basic value display only
- Multi-client support (one browser at a time; second connection replaces first)
- Save state / snapshots
- Network sharing beyond `--listen 0.0.0.0` (no auth, no TLS)

## Risks and unknowns

- **Device main loop coupling.** The interactive loop in `MMBasic.c` may have device-specific calls scattered through it (`mark`, `jmp_buf`, hardware probes) that need stubbing. Phase 0 will surface these.
- **Console text rendering on host.** The on-screen text renderer must already work in host mode if the framebuffer abstraction is real. If not, phase 0 grows.
- **Keyboard semantics.** PicoCalc has Fn-key combinations and a small physical keyboard. Exact mapping to a PC keyboard needs care — what does the browser send for the Fn-equivalent of arrow keys, etc.
- **EDIT in a browser.** Full-screen editor with arrow keys, page up/down — the browser will want to scroll/select instead. Need `preventDefault()` on captured keys, possibly a "capture keyboard" toggle.
- **Audio timing.** WebAudio scheduling is sample-accurate but the WS hop adds jitter. Should be fine for tones; non-issue once we accept it isn't a music engine.

## References

- Existing host build: `host/host_main.c`, `host/host_stubs_legacy.c`, `host/Makefile`, `host/build.sh`
- VM syscall ABI: `vm_sys_*.h`, `bytecode.h`
- Framebuffer abstraction: `host/host_framebuffer_backend.h`
- Mongoose: https://github.com/cesanta/mongoose
