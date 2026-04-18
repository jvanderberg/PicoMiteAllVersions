# Web Host Plan

Compile the macOS host build of PicoMite to WebAssembly so the interpreter + VM run entirely in-browser as a static site. The browser app has a terminal surface for REPL, a canvas for graphics, Web Audio for PLAY, and drag-and-drop / download for file exchange with the user's disk. No server round-trip per keystroke, no backend at all.

- **Branch:** `web-host` (off `host-hal-refactor`).
- **Predecessor plan:** [`host-hal-plan.md`](host-hal-plan.md). That refactor turned the host into a proper HAL consumer (shared `Draw.c`, `FileIO.c`, `Audio.c`, `MM_Misc.c`) with focused HAL modules (`host_runtime.c`, `host_fastgfx.c`, `host_fs_shims.c`, `host_peripheral_stubs.c`, `host_fb.c`, `host_time.c`, `host_terminal.c`). The web host is the third HAL target — macOS native, `--sim` (Mongoose + WebSocket), and now WASM (emscripten + direct JS bridge).
- **Native host is frozen for this work.** `mmbasic_test` and `mmbasic_sim` keep their current termios TTY REPL path, test harness behavior, and build flags. The web-host branch touches only new files (`host_wasm_*.c`, `host/web/*`, `host/build_wasm.sh`, `host/Makefile.wasm`) and a small set of additive `#ifdef MMBASIC_WASM` gates where a narrow WASM-specific behavior is unavoidable. If a change to a shared or native-host file is tempting, that's a signal the abstraction is wrong — fix the WASM HAL instead.
- **Dropping TTY in web-host only.** The WASM target has no termios; `host_terminal.c` simply isn't linked in the WASM build. Keyboard input comes from xterm.js via `wasm_push_key`. `host_terminal.c` itself stays intact and keeps serving the native host's interactive REPL.

## Invariants

1. **Shared source is untouched.** `MMBasic.c`, `Commands.c`, `Functions.c`, `Draw.c`, `FileIO.c`, `Audio.c`, `MM_Misc.c`, the VM (`bc_*.c`, `vm_sys_*.c`), `gfx_*_shared.c`, and `mm_misc_shared.c` compile as-is. The web port is purely a HAL backend swap — anything that required editing a shared file is a bug in the port.
2. **The native host test harness (`mmbasic_test`) stays green.** The WASM target is additive. `cd host/ && ./build.sh && ./run_tests.sh` must pass 201/201 at every phase boundary.
3. **The device build stays green.** No changes to `CMakeLists.txt` / `CMakeLists 2350.txt` or anything under `#ifndef MMBASIC_HOST`.
4. **No server.** The deployable artifact is `index.html` + `picomite.wasm` + `picomite.js` + a preloaded data image. It runs from `file://`, GitHub Pages, or any static host. No COOP/COEP headers required for the MVP (rules out SharedArrayBuffer + pthreads until Phase 7+).
5. **Behavioral parity with the native host.** A `.bas` file that prints X on `mmbasic_test` must print X in the browser. Graphics that render to the native PPM screenshot must render pixel-identical to the canvas. The only legitimate divergences are timing-sensitive ones (cooperative scheduling may change `TIMER` resolution) and peripherals that are no-ops on both ports anyway.

## Target architecture

```
┌──────────────────────────────────────────────────────────────┐
│                      Shared core (unchanged)                 │
│   MMBasic.c, Commands.c, Functions.c, Operators.c, MATHS.c   │
│   MMBasic_REPL.c, MMBasic_Prompt.c, Editor.c                 │
│   Draw.c, FileIO.c, Audio.c, MM_Misc.c, mm_misc_shared.c     │
│   bc_source.c, bc_vm.c, bc_runtime.c, vm_sys_*.c             │
│   gfx_*_shared.c                                             │
├──────────────────────────────────────────────────────────────┤
│                   HAL surface (current)                      │
│   host_runtime.c / host_fs_shims.c / host_fb.c /             │
│   host_fastgfx.c / host_peripheral_stubs.c /                 │
│   host_time.c / mm_misc_shared.c                             │
│   — all compile unchanged under emscripten                   │
├──────────────────────────────────────────────────────────────┤
│             WASM-specific HAL (new, replaces 3 files)        │
│   host_wasm_console.c  ← replaces host_terminal.c            │
│   host_wasm_main.c     ← replaces host_main.c test harness   │
│   host_wasm_bridge.c   ← replaces host_sim_server.c          │
│                          (direct JS ↔ C, no Mongoose)        │
├──────────────────────────────────────────────────────────────┤
│                     Browser surface (JS/HTML)                │
│   index.html + picomite.mjs (loader, glue)                   │
│   ui/terminal.js       — xterm.js wrapper                    │
│   ui/canvas.js         — framebuffer blit, dirty-rect        │
│   ui/audio.js          — Web Audio oscillator/sample pool    │
│   ui/keys.js           — keymap to MMBasic codes             │
│   ui/fs.js             — drag-drop import, download export   │
└──────────────────────────────────────────────────────────────┘
```

### What carries over from the native host, unchanged

| Module | Why it's portable |
|---|---|
| `host_runtime.c` | Lifecycle, `Option`/`FontTable`/`PinDef`/`inttbl` backing globals, error-recovery snapshot — pure C, no OS calls. |
| `host_fastgfx.c` | FASTGFX/FRAMEBUFFER back-buffer RAM allocation + memcpy-based SWAP. No OS, no threading. |
| `host_fb.c` | Pixel plane, rectangles, layers, screenshot (screenshot is trivially redirectable to a canvas `toDataURL`). |
| `host_fs_shims.c` | POSIX FS calls (`open`/`read`/`write`/`opendir`/`readdir`/`stat`/`chdir`/`mkdir`/`unlink`/`rename`/`getcwd`) — emscripten libc provides all of these over MEMFS, which is what we use. |
| `host_peripheral_stubs.c` | No-op I2C/SPI/PWM/PIO/UART/GPIO/ADC — pure stubs, no change. |
| `host_time.c` | `clock_gettime(CLOCK_MONOTONIC)` works under emscripten; `nanosleep` works under ASYNCIFY. |
| `mm_misc_shared.c` | Portable sort/longstring/format/date-time/pause — pure C. |
| VM + compiler (`bc_*.c`) | Already C99, no platform calls. |
| Shared commands (`Draw.c`, `FileIO.c`, `Audio.c`, `MM_Misc.c`) | Host HAL refactor already gated every device branch with `#ifdef MMBASIC_HOST`. |

### What needs a new backend

| Native host module | WASM replacement | Reason |
|---|---|---|
| `host_terminal.c` (termios raw-mode stdin, unbuffered stdout) | `host_wasm_console.c` — JS-queue-backed `MMInkey` + `EM_JS` calls to xterm.js writer | No termios in browsers. |
| `host_main.c` (CLI test harness, argv parsing, oracle compare) | `host_wasm_main.c` — exported `wasm_boot()` / `wasm_tick()` entry points wired to `emscripten_set_main_loop` | No argv, no compare-mode; main loop must yield cooperatively. |
| `host_sim_server.c` (Mongoose HTTP + WebSocket server, pthread tick) | `host_wasm_bridge.c` — direct `EM_JS` / exported functions (`wasm_push_key`, `wasm_drain_graphics_cmds`, `wasm_audio_event`) | Browser talks to the wasm module via postMessage/JS imports, not WebSockets. |
| `vendor/mongoose.c` | **Dropped entirely.** | Not needed without an in-process network server. |
| `host_sim_audio.c` JSON emitter | `host_wasm_audio.c` — same shape, but calls `EM_JS` directly into a Web Audio queue (no JSON marshaling) | Type-safe, zero-copy. |

## How the emscripten build fits in

The WASM port is a new make target alongside `mmbasic_test` and `mmbasic_sim`. All three share `CORE_SRCS`; they diverge only in the HAL modules linked.

- **Toolchain:** emscripten 3.1.x or later (`emcc`, `emmake`, `emrun`). Pinned in `host/build_wasm.sh` and CI.
- **Command shape:**
  ```sh
  emcc -O2 \
    -DPICOMITE -DMMBASIC_HOST -DMMBASIC_WASM \
    -include host_platform.h \
    -sASYNCIFY=1 -sASYNCIFY_STACK_SIZE=65536 \
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 \
    -sEXPORTED_FUNCTIONS='["_main","_wasm_boot","_wasm_push_key","_wasm_tick","_wasm_framebuffer_ptr","_wasm_framebuffer_width","_wasm_framebuffer_height","_malloc","_free"]' \
    -sEXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAPU8","FS"]' \
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web \
    -lidbfs.js \
    --preload-file tests@/sd \
    $(CORE_SRCS) $(WASM_HAL_SRCS) \
    -o web/picomite.mjs
  ```
- **Size target:** ~1.5 MB uncompressed `.wasm`, ~600–800 KB gzipped. Verified in CI.
- **MMBASIC_WASM macro** — new narrow gate for the handful of spots that must differ from native (e.g. `nanosleep` → `emscripten_sleep`, console output → `EM_ASM`). Set in addition to `MMBASIC_HOST`, never instead of it.

### Execution model

The interpreter owns the main loop (`ExecuteProgram` → statement loop → periodic `CheckAbort` / `MMInkey` poll). In a browser, blocking forever freezes the tab.

**MVP: ASYNCIFY.** Emscripten's ASYNCIFY transforms the generated code so that marked "sleep" points unwind the C stack, return to the JS event loop, and resume on the next tick. We mark:

- `host_sleep_us` (already the single entry point for all `PAUSE` / `DELAY` / `nanosleep`)
- `host_sync_msec_timer` (called on every interpreter poll checkpoint — yields 1 tick worth of frame time)
- `MMInkey` when the queue is empty (optional — only if `INPUT$` / `EDIT` need it; most polled reads return immediately)

ASYNCIFY adds ~30% runtime cost and ~15% binary size. Acceptable for MVP. Pay attention to which call sites need `await`-able behavior; over-marking bloats the transform.

**Post-MVP: worker + Atomics.** The interpreter moves to a Web Worker. The main thread owns the DOM (xterm.js, canvas, audio). Communication is `postMessage` for bulk data and `Atomics.wait`/`Atomics.notify` on a `SharedArrayBuffer` ring for sub-millisecond input latency. Requires COOP/COEP response headers, which rules out naive GitHub Pages deployment (need `_headers` on Netlify / Cloudflare Pages / custom hosting). Defer until ASYNCIFY proves inadequate.

## Phased plan

Each phase ends with a green native host build, a green device build, a green WASM build, and a commit. No phase depends on the next.

### Phase 0 — Toolchain scaffolding ✅ (2026-04-18)

**Goal:** `emcc hello.c` builds and serves; the native build still passes.

- Add `host/build_wasm.sh` that installs/activates emscripten (or assumes `EMSDK` env) and invokes `emmake make -f Makefile.wasm`.
- Add `host/Makefile.wasm` — mirrors `host/Makefile` but with the `CORE_SRCS` pared to just enough to prove linking (REPL + interpreter, no graphics, no audio).
- Add `host/web/` with `index.html`, `picomite.css`, and a placeholder `app.mjs` that imports the compiled module and writes "Hello from PicoMite WASM" to a `<pre>`.
- Add a `host/web/serve.sh` that runs `python3 -m http.server 8000 --directory web/` (or equivalent) for local smoke testing.

**Exit gate:** `./build_wasm.sh && ./serve.sh` → opening `http://localhost:8000` shows the banner. Native `./build.sh && ./run_tests.sh` still green.

**Landed:** `host/hello_wasm.c` (trivial `printf` main), `host/Makefile.wasm` (MODULARIZE+ES6, ALLOW_MEMORY_GROWTH, INITIAL_MEMORY=32 MiB, SRCS is just `hello_wasm.c` for now), `host/build_wasm.sh` (sources `~/emsdk/emsdk_env.sh` if emcc isn't already on PATH), `host/web/{index.html,picomite.css,app.mjs,serve.sh,.gitignore}`. Loading the page in headless Chromium renders `Hello from PicoMite WASM` in `#out` with zero console/page errors. Native `./run_tests.sh` stays at 201/201.

### Phase 1 — REPL over xterm.js

**Goal:** Type `PRINT 2+3`, see `5` in a browser terminal.

- Write `host/host_wasm_console.c`:
  - Drop-in for `host_terminal.c` — same exported API (`host_raw_mode_enter/exit`, `host_read_byte_nonblock`, `host_read_byte_blocking_ms`).
  - Backed by an in-module ring buffer. JS calls `wasm_push_key(code)` via `ccall` on keypress.
  - `MMputchar` / `MMPrintString` route through `EM_ASM({ window.picomiteTerm.write(UTF8ToString($0)); }, buf)` — or an exported `wasm_output_drain()` polled from JS each frame if that turns out faster.
  - `setvbuf(stdout, NULL, _IONBF, 0)` unchanged (emscripten stdout routes to `console.log` by default; we override `Module.print`).
- Write `host/host_wasm_main.c`:
  - Exports `wasm_boot()` — wraps the `main()` body: `InitialiseAll()` → `MMBasic_REPL()`.
  - Under ASYNCIFY, `MMBasic_REPL`'s polled read loop yields via `emscripten_sleep(0)` in `host_read_byte_blocking_ms`.
  - No `emscripten_set_main_loop` yet — ASYNCIFY gives us a pseudo-synchronous main from JS's view.
- Write `host/web/app.mjs`:
  - Loads xterm.js (CDN or bundled), attaches to `#terminal`.
  - On `onData`, iterates UTF-8 code points and calls `wasm_push_key(code)`.
  - Implements `Module.print = (s) => term.write(s + "\n"); Module.printErr = ...`.
  - Calls `Module._wasm_boot()` after `Module` resolves.

**Exit gate:** Browser shows `> ` prompt. Typing `PRINT 2+3` ENTER prints `5`. `LIST`, `NEW`, simple `FOR`/`NEXT` loops all work. No graphics, no audio, no file I/O yet. Tab does not freeze on `PAUSE 100`.

### Phase 2 — Filesystem (preload bundle + drag-drop import + download export)

**Goal:** `FILES`, `LOAD "t001.bas"`, `RUN`, `SAVE` work. User brings their own files in by dragging onto the page; takes them home by downloading.

File access is deliberately transient MEMFS-only. No IDBFS, no OPFS, no File System Access API. Files live in the page's memory for the session; user is responsible for exporting anything they want to keep. Matches how one would use the native host: programs come from disk (here, from drag-drop) and leave to disk (here, via download). Simple, universal, no quotas, no permission prompts.

- Add `--preload-file demos@/sd` to the emscripten command line. The `demos/` directory holds a curated set of bundled examples (Mandelbrot, graphics demos, small games) — read-only from the user's perspective, always present at `/sd/` on boot. Not the full test corpus; we pick maybe 10–20 representative programs.
- `host_sd_root = "/sd"` at boot so `host_fs_shims.c` routes POSIX through emscripten's MEMFS. No C code change — already wired.
- `host/web/ui/fs.js` — three small surfaces:
  - **Drop zone:** the whole page is a `dragover`/`drop` target. Dropped files are read via `FileReader.readAsArrayBuffer`, then `FS.writeFile('/sd/' + file.name, new Uint8Array(buf))`. Multiple files OK. On completion, a toast says "Loaded foo.bas — type `FILES` to list".
  - **Download current program:** a "⬇ Download" button grabs the current program via an exported `wasm_current_program()` (or reads the last-`SAVE`-d file from `/sd/`) and triggers a browser download via `new Blob([bytes]); URL.createObjectURL; <a download>`. Also: any `SAVE "name.bas"` can optionally auto-trigger the download if a "Save also downloads" checkbox is ticked, so power users don't have to click twice.
  - **Download all:** a "⬇ All as .zip" button packs the contents of `/sd/` (minus the preloaded demos, if we track that) into a zip via a tiny zip library (e.g. `fflate`, ~8 KB) and downloads it. Useful for bulk export.
- No C code changes. All logic is JS; from the interpreter's view, `/sd/` is just a POSIX directory.

**Exit gate:** `FILES` lists the bundled demos. `RUN "mandelbrot.bas"` works. Drag a local `.bas` file onto the page → `FILES` shows it → `RUN "mine.bas"` works. `SAVE "new.bas"` → click "Download" → `new.bas` lands in the user's Downloads folder. Refresh the page → drag-dropped files are gone (expected), bundled demos still there (expected).

**Notes:**
- No persistence across reloads is a *feature*, not a bug, for this iteration. It makes the mental model obvious ("the page is a sandbox; save to your disk to keep anything") and avoids a pile of edge cases (quota exceeded, stale OPFS state, browser deleting your files after 60 days of inactivity, etc.).
- If persistence becomes desirable later, it slots in as a post-MVP phase: mount OPFS at `/home/` and add a "Copy to persistent storage" action. The `host_fs_shims.c` routing doesn't care which MEMFS-equivalent backend is at a given path.
- Upload from a `<input type="file" multiple>` picker is a nice addition alongside drag-drop for mobile / touch where drag-drop is awkward. Same underlying path (`FS.writeFile`). Trivial to add; mention in the Phase 2 follow-on list.

### Phase 3 — Graphics to canvas

**Goal:** `CIRCLE`, `LINE`, `PSET`, `BLIT`, FASTGFX demos all render visibly.

- Add `<canvas id="screen" width="320" height="320">` to `index.html`, styled with `image-rendering: pixelated`.
- Write `host/host_wasm_canvas.c`:
  - Exports `wasm_framebuffer_ptr()` → returns `host_framebuffer` (the 24-bpp RGB plane from `host_fb.c`).
  - Exports `wasm_framebuffer_width()` / `wasm_framebuffer_height()`.
  - Exports `wasm_dirty_rect()` → returns a packed `{x,y,w,h}` and resets the accumulated dirty region.
- Extend `host_fb.c` with a minimal dirty-rect accumulator (`host_fb_dirty_add(x,y,w,h)` called from `DrawPixel`, `DrawRectangle`, `ScrollLCD`, `FASTGFX SWAP`). The native host ignores it; WASM uses it.
- Write `host/web/ui/canvas.js`:
  - On each `requestAnimationFrame`, call `Module._wasm_dirty_rect()` → if non-empty, read `HEAPU8.subarray(fbPtr + y*w*3, ...)` → convert RGB24 to RGBA32 → `ctx.putImageData(...)`.
  - Handle FASTGFX SWAP events (the existing `host_sim_server`-style event, but delivered directly via an exported queue).
- Ensure `host_runtime_begin` still wires `DrawPixel`/`DrawRectangle`/`ScrollLCD`/`DrawBitmap` to the existing `host_fb_*` functions — no change needed from current host HAL.

**Exit gate:** A test that draws concentric circles shows concentric circles on the canvas, pixel-identical to the PPM screenshot produced by the native test harness. FASTGFX demo (e.g. a bouncing ball) runs at 60 FPS. `BLIT` from back buffer works.

### Phase 4 — Audio via Web Audio

**Goal:** `PLAY TONE 440,1000` emits a 440 Hz beep for 1 s.

- Write `host/host_wasm_audio.c` (parallel to `host_sim_audio.c`):
  - Replace each JSON-emit call with an `EM_ASM` invocation that calls into a JS audio bus (`window.picomiteAudio.tone(freq, ms)`, `.sound(waveform, freq, duration, volume)`, etc.).
  - `PLAY STOP`, `PLAY PAUSE`, `PLAY RESUME` map to matching JS methods.
- Write `host/web/ui/audio.js`:
  - Lazy-create `AudioContext` on first user gesture (otherwise browser blocks).
  - `tone(freq, ms)` → `OscillatorNode` with ramped envelope, scheduled via `audioCtx.currentTime`.
  - `sound(waveform, freq, duration, volume)` → 4 channels multiplexed (matching `PLAY SOUND` semantics).
  - No MOD/WAV/MP3 yet — scope those to Phase 4b or later.
- Display a banner "Click anywhere to enable audio" until the user gesture unblocks the audio context.

**Exit gate:** A test program playing a melody (PLAY TONE + PAUSE sequence) produces audible output. Four-voice `PLAY SOUND` chord sounds right. Audio does not stutter during graphics-heavy programs.

### Phase 5 — Cooperative scheduling & timing

**Goal:** `TIMER`, `PAUSE`, `FASTGFX SYNC`, cursor blink all behave like the native port.

- Mark `host_sleep_us` and `host_sync_msec_timer` as ASYNCIFY entry points in the link flags (`-sASYNCIFY_IMPORTS`).
- Under `MMBASIC_WASM`, replace the `nanosleep` body of `host_sleep_us` with `emscripten_sleep(us / 1000)`.
- Add a 1 kHz tick: JS's `setInterval(() => Module._wasm_tick(), 1)` (or `AudioWorkletNode` for jitter-free ticks). The exported `wasm_tick()` calls `host_sync_msec_timer()` once.
  - Alternative: drive the tick from within `host_sleep_us` itself and drop the JS interval. Prefer this — fewer moving parts — unless programs that never sleep also need `TIMER` to advance (they do; measure and decide).
- Verify that `PAUSE 1000` yields cleanly: the browser's event loop runs, `mousemove` events fire, the canvas redraws. No "page unresponsive" dialog.
- Verify `FASTGFX SYNC` blocks until vsync aligns with 60 Hz — run `requestAnimationFrame` hooks through the event loop while ASYNCIFY sleeps.

**Exit gate:** `timer_test.bas` reports monotonically increasing `TIMER` values. `PAUSE 5000` in a tight loop keeps the tab responsive. Cursor blinks at the correct rate. FASTGFX demos measure 60 FPS with no stutter.

### Phase 6 — Input polish

**Goal:** Every key MMBasic cares about arrives with the right code; Ctrl-C breaks programs; optional mouse support.

- `host/web/ui/keys.js` — translation table from JS `KeyboardEvent.key` / `.code` to MMBasic key codes. Cover arrows, F1–F12, Home/End/PgUp/PgDn, Ins/Del, Esc, Tab, Backspace, Enter, Ctrl-letter combinations.
- Capture Ctrl-C at the JS level and set an exported `wasm_break_flag = 1` that the interpreter polls during `CheckAbort`. Don't let the browser's "copy" shortcut swallow it in terminal focus.
- Optional: hook `mousemove` / `mousedown` on the canvas to a `host_wasm_mouse_x/y/buttons` global read by a future `MOUSE` function (out of scope for MVP).

**Exit gate:** `EDIT` works with arrow keys. `INPUT$(1)` returns correct codes for Ctrl-A, Esc, Tab, function keys. Ctrl-C breaks a running `FOR I=1 TO 1e9: NEXT`.

### Phase 7 — Build, package, deploy

**Goal:** `npm run build` (or equivalent) produces a deployable static bundle; CI pushes it on every merge to `web-host`.

- Split `build_wasm.sh` into debug (`-O1 -g -s ASSERTIONS=2`) and release (`-O2 -s ASSERTIONS=0 --closure 1`) variants. Release is what ships.
- Add a GitHub Actions workflow `.github/workflows/wasm.yml`:
  - Sets up emscripten (`mymindstorm/setup-emsdk@v14` or equivalent pinned action).
  - Runs `host/build_wasm.sh release`.
  - Uploads `host/web/` as a Pages artifact.
  - On `main` merges of the eventual feature branch, publishes to GitHub Pages.
- Add a `host/web/README.md` explaining local dev flow (`./build_wasm.sh debug && ./serve.sh`).
- Size budget: gzipped `.wasm` must stay under 1 MB; CI fails if it exceeds.

**Exit gate:** `picomite.github.io/web` (or whatever the URL turns out to be) loads the REPL, runs `tests/t001.bas` successfully, renders a graphics demo, and plays a tone, all from a fresh browser cache.

## Key risks

1. **ASYNCIFY overhead.** +30% runtime cost and +15% binary size, applied globally once any call site is marked. If the interpreter feels sluggish in benchmarks, the escape hatch is the Phase 8 worker rearchitecture (see below). Measure early — by end of Phase 1 — against the native host for a reasonably long program (a 10 s Mandelbrot). If the gap is >2×, escalate.
2. **pthreads / SharedArrayBuffer gated by COOP/COEP.** The MVP deliberately avoids them. GitHub Pages does not send COOP/COEP headers. Moving to Cloudflare Pages or Netlify with a `_headers` file unblocks pthreads whenever needed — just mark it as a deployment change, not a rewrite.
3. **Binary size.** The full VM + interpreter + graphics + audio + VM command table is large. Compile with `-Oz` if `-O2` ships too fat; strip `vm_sys_fft_table.c`-style precomputed tables if they dominate; consider split output (core + async-loaded sample library) if first-load budget is blown.
4. **Audio autoplay gate.** No audio plays until the user clicks. Make the failure mode obvious (visible banner), not silent (tone scheduled, nothing happens).
5. **Timing drift under ASYNCIFY.** `emscripten_sleep(1)` is lower-bounded by the event-loop latency (often 4 ms on throttled tabs). `PAUSE 1` won't be 1 ms. `TIMER` will still advance monotonically via `performance.now()`, so end-to-end behavior stays correct — but busy-wait timing tricks in `.bas` programs may misbehave. Document.
6. **No persistence by design.** User drags files in, saves by downloading. A page reload loses anything they didn't download. Communicate this clearly in the UI (banner on first load, tooltip on the REPL). If users complain, add OPFS as a post-MVP persistence layer — but start simple.
7. **Copy/paste and keyboard focus.** xterm.js handles this well, but Ctrl-C as "break" vs. Ctrl-C as "copy selected text" collides. Resolve by routing Ctrl-C to break when no selection is active, to clipboard when text is selected.

## Open questions (resolve during Phase 0 / 1)

- **ASYNCIFY vs. `emscripten_set_main_loop` as the primary yield mechanism?** ASYNCIFY is easier on existing code; main-loop refactor is cleaner but requires teasing apart `ExecuteProgram` into a re-entrant state machine. Start ASYNCIFY; measure; revisit if needed.
- **xterm.js vs. plain `<textarea>` vs. custom terminal widget?** xterm.js is the obvious pick — standard VT100, copy/paste, themes — but it's ~300 KB. If we care about total footprint more than UX polish, a minimal custom widget is viable.
- **Preload which tests?** The native harness ships 201 `.bas` files; bundling them all is ~400 KB uncompressed. Preload the interesting demos; leave the regression suite as a separate preload file gated by a dev flag.
- **Do we expose the VM-vs-interpreter compare mode in the browser?** Probably not — it's a dev tool. But exposing `?mode=vm` / `?mode=interp` query params for forcing one or the other could help debugging.
- **ES module (MJS) vs. classic script?** Default to MJS (`EXPORT_ES6=1`) for cleaner integration with modern tooling. Plain script is fallback if deployment target rejects modules.

## Post-MVP follow-ons

- **Phase 8: Worker + Atomics.** Move the interpreter to a Web Worker. Drop ASYNCIFY. Main thread owns DOM + audio. `SharedArrayBuffer` ring for input, framebuffer, audio samples. `Atomics.wait`/`notify` for blocking. Requires COOP/COEP — plan the deploy change simultaneously.
- **Phase 9: PLAY WAV / MOD / MP3.** Decode in JS with `decodeAudioData`, schedule via `AudioBufferSourceNode`. Non-trivial — needs a buffering strategy compatible with `PLAY PAUSE`/`RESUME`.
- **Phase 10: Mobile / touch.** Virtual keyboard overlay, pinch-to-zoom on canvas, gesture mapping to MMBasic `MOUSE`.
- **Phase 11: Offline PWA.** Service worker, installable, works without internet once cached. Easy once the static bundle stabilizes.
- **Phase 12: Multi-program tabs.** Multiple VM instances (one per worker) sharing the same canvas or with independent canvases. Mostly a UI exercise.

## Rollout sequence

1. Land this plan document on `web-host` branch.
2. Execute Phase 0 (scaffolding) + Phase 1 (REPL) — single PR to `web-host`. This is the riskiest PR because it proves or disproves ASYNCIFY as the scheduling strategy.
3. Phase 2 (FS), Phase 3 (graphics), Phase 4 (audio), Phase 5 (scheduling), Phase 6 (input) — one PR each.
4. Phase 7 (deploy) once everything works locally.
5. Merge `web-host` → `main`. Promote the docs / link from the main README.
6. Iterate on post-MVP items as user feedback comes in.

## Success criteria

- Any `.bas` file in the existing test corpus that doesn't touch hardware peripherals or require >32 MB of heap runs in the browser with visually and audibly identical behavior to the native host.
- Cold page load to first prompt: under 2 seconds on a modern laptop over a fast connection.
- Memory footprint: under 64 MB for an idle REPL session.
- Zero backend dependencies: a static bundle dropped in `s3://anywhere` is the complete deployable.
