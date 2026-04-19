// MMBasic Web loader.
//
// Boots the WASM module, wires keyboard → wasm_push_key, blits the
// host_fb framebuffer onto #screen on every requestAnimationFrame, and
// exposes file I/O via drag-drop + upload button + download-all.
//
// Files live under /sd/ in emscripten's MEMFS. Bundled demos are
// packed into picomite.data via --preload-file demos@/sd. Drag-drop
// and the upload button write fresh files into /sd/. The download
// button packages every .bas-ish file under /sd/ as a ZIP.
//
// Persistence is intentional: MEMFS resets on reload. Users who want
// to keep work save via the download button. See web-host plan
// "No persistence by design" risk.

import Module from './picomite.mjs';

const statusEl = document.getElementById('status');
const canvas = document.getElementById('screen');
const uploadInput = document.getElementById('upload');
const downloadBtn = document.getElementById('download-sd');
const resetBtn = document.getElementById('reset-sd');
const openConfigBtn = document.getElementById('open-config');
const configDialog = document.getElementById('config-dialog');
const configCloseBtn = document.getElementById('config-close');
const resolutionSelect = document.getElementById('resolution');
const memorySelect = document.getElementById('memory');
const slowdownRange = document.getElementById('slowdown-range');
const slowdownNumber = document.getElementById('slowdown-number');
const dropOverlay = document.getElementById('drop-overlay');
const hintEl = document.getElementById('hint');

// WebGL path. Canvas 2D's putImageData committed frames through the
// software compositor path, which — on macOS Chrome specifically —
// caused rAF throttling from 50 Hz down to 25 Hz after ~8 seconds of
// held keys (documented in web-host-plan.md Phase 4 notes). WebGL's
// commit goes straight to the GPU compositor and Chrome's throttler
// doesn't fire. Firefox and Safari work fine either way, but the
// WebGL path is also faster (~3× less CPU per frame) so we standardise
// on it.
//
// `desynchronized: true` lets the browser present frames with lower
// queueing latency when supported. `preserveDrawingBuffer: false`
// and `antialias: false` shave redundant work.
const gl = canvas.getContext('webgl2', {
    alpha: false,
    antialias: false,
    depth: false,
    stencil: false,
    preserveDrawingBuffer: false,
    desynchronized: true,
    powerPreference: 'high-performance',
});
if (!gl) {
    document.getElementById('status').textContent = 'WebGL2 not available in this browser.';
    throw new Error('WebGL2 required');
}
gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);  // our plane is top-down

// Shader pair: draw a fullscreen triangle, sample the framebuffer
// texture, and swizzle R↔B in the fragment stage. host_framebuffer
// stores pixels as (R<<16)|(G<<8)|B in a uint32 plane, which when
// uploaded as RGBA bytes lands as (B, G, R, 0) per texel — so the
// shader reads .bgr to recover the correct colour.
const VERT_SRC = `#version 300 es
in vec2 a_pos;
out vec2 v_uv;
void main() {
    v_uv = a_pos * 0.5 + 0.5;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}`;
const FRAG_SRC = `#version 300 es
precision mediump float;
in vec2 v_uv;
uniform sampler2D u_tex;
out vec4 outColor;
void main() {
    vec4 c = texture(u_tex, v_uv);
    outColor = vec4(c.b, c.g, c.r, 1.0);
}`;

function compileShader(type, src) {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
        throw new Error('Shader compile: ' + gl.getShaderInfoLog(s));
    }
    return s;
}

const glProgram = gl.createProgram();
gl.attachShader(glProgram, compileShader(gl.VERTEX_SHADER,   VERT_SRC));
gl.attachShader(glProgram, compileShader(gl.FRAGMENT_SHADER, FRAG_SRC));
gl.linkProgram(glProgram);
if (!gl.getProgramParameter(glProgram, gl.LINK_STATUS)) {
    throw new Error('Shader link: ' + gl.getProgramInfoLog(glProgram));
}
gl.useProgram(glProgram);

// Two-triangle fullscreen quad. Could be a single triangle with
// out-of-bounds UVs, but the explicit quad is easier to read and the
// vertex cost is negligible.
const quadVB = gl.createBuffer();
gl.bindBuffer(gl.ARRAY_BUFFER, quadVB);
gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
    -1, -1,   1, -1,   -1,  1,
    -1,  1,   1, -1,    1,  1,
]), gl.STATIC_DRAW);
const aPosLoc = gl.getAttribLocation(glProgram, 'a_pos');
gl.enableVertexAttribArray(aPosLoc);
gl.vertexAttribPointer(aPosLoc, 2, gl.FLOAT, false, 0, 0);
gl.uniform1i(gl.getUniformLocation(glProgram, 'u_tex'), 0);

// Texture — allocated when we know the framebuffer size.
const glTex = gl.createTexture();
gl.activeTexture(gl.TEXTURE0);
gl.bindTexture(gl.TEXTURE_2D, glTex);
gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

// --- Resolution selection -----------------------------------------------
//
// Framebuffer size must be set BEFORE wasm_boot (the allocation happens
// during host_runtime_begin). We can't re-init the interpreter
// mid-session safely — setjmp/longjmp state makes it fragile — so
// resolution changes trigger a full page reload with ?res=WxH.
//
// Priority: URL query > localStorage > default.

const DEFAULT_RES = '320x320';
const RES_KEY = 'picomite.res';

function parseRes(s) {
    const m = /^(\d+)x(\d+)$/.exec(String(s || '').trim());
    if (!m) return null;
    const w = parseInt(m[1], 10), h = parseInt(m[2], 10);
    if (w < 80 || h < 60 || w > 2048 || h > 2048) return null;
    return { w, h, label: `${w}x${h}` };
}

function pickResolution() {
    const params = new URLSearchParams(window.location.search);
    return parseRes(params.get('res'))
        ?? parseRes(localStorage.getItem(RES_KEY))
        ?? parseRes(DEFAULT_RES);
}

function applyResolutionSelect(label) {
    const opt = Array.from(resolutionSelect.options).find(o => o.value === label);
    if (opt) {
        resolutionSelect.value = label;
    } else {
        // Custom value from URL param — inject a matching option so the
        // dropdown reflects the current state.
        const custom = document.createElement('option');
        custom.value = label;
        custom.textContent = label.replace('x', ' × ') + ' (custom)';
        resolutionSelect.appendChild(custom);
        resolutionSelect.value = label;
    }
}

function reloadWithResolution(label) {
    try { localStorage.setItem(RES_KEY, label); } catch (_) {}
    const url = new URL(window.location.href);
    url.searchParams.set('res', label);
    window.location.href = url.toString();
}

// --- Memory selection ----------------------------------------------------

const DEFAULT_MEM = 2097152;           // 2 MB — generous web default
const MEM_KEY = 'picomite.mem';
const MEM_MIN = 32 * 1024;
const MEM_MAX = 8 * 1024 * 1024;       // matches HEAP_MEMORY_SIZE ceiling in configuration.h

function parseMem(s) {
    const n = parseInt(String(s || '').trim(), 10);
    if (!Number.isFinite(n)) return null;
    if (n < MEM_MIN || n > MEM_MAX) return null;
    return n;
}

function pickMemory() {
    const params = new URLSearchParams(window.location.search);
    return parseMem(params.get('mem'))
        ?? parseMem(localStorage.getItem(MEM_KEY))
        ?? DEFAULT_MEM;
}

function applyMemorySelect(bytes) {
    // If the exact byte count isn't in the option list, synth a custom
    // entry that preserves the URL-provided value.
    const opt = Array.from(memorySelect.options).find(o => +o.value === bytes);
    if (opt) {
        memorySelect.value = String(bytes);
    } else {
        const custom = document.createElement('option');
        custom.value = String(bytes);
        custom.textContent = `${Math.round(bytes / 1024)} KB (custom)`;
        memorySelect.appendChild(custom);
        memorySelect.value = String(bytes);
    }
}

function reloadWithMemory(bytes) {
    try { localStorage.setItem(MEM_KEY, String(bytes)); } catch (_) {}
    const url = new URL(window.location.href);
    url.searchParams.set('mem', String(bytes));
    window.location.href = url.toString();
}

// --- Slowdown selection -------------------------------------------------
//
// Applies live via wasm_set_slowdown_us — no reload needed. Unit is
// MICROSECONDS, matching the native --slowdown flag. In the browser the
// raw per-statement sleep floors to 1 ms (ASYNCIFY unwinds to the event
// loop), so host_sim_apply_slowdown on WASM uses an accumulator: small
// µs values translate to "sleep 1 ms every N statements". That gives
// true sub-ms average pacing without the 1-ms-per-statement cliff we
// had when we treated the UI value as ms.
//
// Priority: URL query > localStorage > 0 (uncapped).

const DEFAULT_SLOWDOWN_US = 0;
const SLOW_KEY = 'picomite.slowdown.us';
const SLOW_MAX_US = 100000;  // 100 ms per statement — silly ceiling

function parseSlowdown(s) {
    const n = parseInt(String(s ?? '').trim(), 10);
    if (!Number.isFinite(n)) return null;
    if (n < 0 || n > SLOW_MAX_US) return null;
    return n;
}

function pickSlowdown() {
    const params = new URLSearchParams(window.location.search);
    return parseSlowdown(params.get('slow'))
        ?? parseSlowdown(localStorage.getItem(SLOW_KEY))
        ?? DEFAULT_SLOWDOWN_US;
}

function applySlowdownInputs(us) {
    slowdownNumber.value = String(us);
    slowdownRange.value = String(Math.min(us, parseInt(slowdownRange.max, 10)));
}

let applySlowdownLive = () => {};  // wired up once the wasm instance exists

function setStatus(text, isError = false) {
    statusEl.textContent = text;
    statusEl.classList.toggle('error', isError);
}

// --- MMBasic key-code mapping --------------------------------------------

const SPECIAL_KEYS = {
    Enter:      0x0D,
    Tab:        0x09,
    Backspace:  0x08,
    Delete:     0x7F,
    Escape:     0x1B,
    ArrowUp:    0x80,
    ArrowDown:  0x81,
    ArrowLeft:  0x82,
    ArrowRight: 0x83,
    Insert:     0x84,
    Home:       0x86,
    End:        0x87,
    PageUp:     0x88,
    PageDown:   0x89,
};

function mapKeyEvent(event) {
    const k = event.key;
    if (SPECIAL_KEYS[k] !== undefined) return SPECIAL_KEYS[k];
    if (/^F([1-9]|1[0-2])$/.test(k)) return 0x90 + parseInt(k.slice(1), 10);
    if (k.length === 1) {
        const code = k.charCodeAt(0);
        if (event.ctrlKey && code >= 0x40 && code < 0x80) return code & 0x1F;
        if (code < 0x80) return code;
    }
    return -1;
}

// --- Framebuffer blit ----------------------------------------------------

let fbPtr = 0, fbWidth = 0, fbHeight = 0;
let fbTexAllocated = false;  // first upload uses texImage2D, rest texSubImage2D

// Render at 2× when it fits the viewport, otherwise shrink uniformly
// to fit while preserving aspect ratio. image-rendering: pixelated
// keeps things crisp even at non-integer scales.
const DESIRED_SCALE = 2;
const VIEWPORT_CHROME_PX = 140;

function fitCanvas() {
    if (!fbWidth || !fbHeight) return;
    const maxW = Math.max(160, Math.floor(window.innerWidth * 0.92));
    const maxH = Math.max(160, window.innerHeight - VIEWPORT_CHROME_PX);
    const fit = Math.min(maxW / fbWidth, maxH / fbHeight);
    const scale = Math.min(DESIRED_SCALE, fit);
    canvas.style.width  = `${Math.floor(fbWidth  * scale)}px`;
    canvas.style.height = `${Math.floor(fbHeight * scale)}px`;
}

// Upload host_framebuffer into the GL texture and draw a fullscreen
// quad. The shader swizzles the R/B channels so the host's (R<<16)|
// (G<<8)|B packed layout displays correctly. On first call we use
// texImage2D to allocate storage; subsequent calls reuse via
// texSubImage2D (cheaper — driver keeps the same GPU allocation).
function blitFrame(instance) {
    if (!fbPtr) return;
    const bytes = new Uint8Array(instance.HEAPU8.buffer, fbPtr, fbWidth * fbHeight * 4);
    if (!fbTexAllocated) {
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, fbWidth, fbHeight, 0,
                      gl.RGBA, gl.UNSIGNED_BYTE, bytes);
        fbTexAllocated = true;
    } else {
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, fbWidth, fbHeight,
                         gl.RGBA, gl.UNSIGNED_BYTE, bytes);
    }
    gl.drawArrays(gl.TRIANGLES, 0, 6);
}

function startRenderLoop(instance) {
    let lastGen = 0xFFFFFFFF;

    const loop = () => {
        try {
            // Skip putImageData entirely if the framebuffer hasn't
            // changed. Covers idle REPL, long PAUSEs, INKEY$ spin-
            // waits — the main thread stays free for keyboard, audio,
            // and IDBFS work.
            const gen = instance._wasm_framebuffer_generation();
            if (gen !== lastGen) {
                lastGen = gen;
                blitFrame(instance);
            }
        } catch (e) {
            setStatus('Render error: ' + e.message, true);
            return;
        }
        requestAnimationFrame(loop);
    };
    requestAnimationFrame(loop);
}

// --- Keyboard ------------------------------------------------------------

// Browser keydown auto-repeat fires ~25-35 Hz on most OSes — 4-5×
// faster than PicoMite's own key-repeat logic (device defaults
// 600 ms / 150 ms = 6.7 Hz). That made pico_blocks' paddle unplayably
// fast on the web. We ignore the browser's auto-repeats and drive our
// own schedule per physical key via plain setTimeout + setInterval.
// (An earlier rAF-based repeat loop looked cleaner on paper but
// added per-event cost in Chrome that manifested as paddle stutter.)
const KEY_REPEAT_DELAY_MS    = 400;
const KEY_REPEAT_INTERVAL_MS = 100;

function wireKeyboard(instance) {
    const pushKey = instance.cwrap('wasm_push_key', null, ['number']);

    // code (event.code) -> { delayTimer, intervalTimer, mmCode }
    const held = new Map();

    const releaseAll = () => {
        for (const s of held.values()) {
            if (s.delayTimer)    clearTimeout(s.delayTimer);
            if (s.intervalTimer) clearInterval(s.intervalTimer);
        }
        held.clear();
    };

    // Passive listener: tells Chrome it can commit compositor frames
    // without waiting for this handler to return (or for a possible
    // preventDefault). Non-passive keydown handlers on macOS Chrome
    // cause the compositor to throttle rAF from the display rate to
    // half-rate during continuous key-hold — see trace analysis in
    // docs/web-host-plan.md's Phase 4 notes. Cost of passive: we
    // can't preventDefault scrolling from arrow keys, but the canvas
    // has no scroll and the page has no fallback target.
    canvas.addEventListener('keydown', (event) => {
        const mm = mapKeyEvent(event);
        if (mm < 0) return;
        // We drive our own repeat — ignore the OS-level auto-repeats.
        if (event.repeat) return;

        pushKey(mm);

        // Clear any stale entry (e.g. keyup was missed because focus
        // moved during the press — rare but ugly if left unhandled).
        const stale = held.get(event.code);
        if (stale) {
            if (stale.delayTimer)    clearTimeout(stale.delayTimer);
            if (stale.intervalTimer) clearInterval(stale.intervalTimer);
        }

        const state = { delayTimer: 0, intervalTimer: 0, mmCode: mm };
        state.delayTimer = setTimeout(() => {
            state.delayTimer = 0;
            state.intervalTimer = setInterval(() => {
                pushKey(state.mmCode);
            }, KEY_REPEAT_INTERVAL_MS);
        }, KEY_REPEAT_DELAY_MS);
        held.set(event.code, state);
    }, { passive: true });

    canvas.addEventListener('keyup', (event) => {
        const s = held.get(event.code);
        if (!s) return;
        if (s.delayTimer)    clearTimeout(s.delayTimer);
        if (s.intervalTimer) clearInterval(s.intervalTimer);
        held.delete(event.code);
    }, { passive: true });

    // Focus changes can eat keyup events. Drop all held keys on blur
    // so the game doesn't see a stuck repeat when the user comes back.
    window.addEventListener('blur', releaseAll);
    canvas.addEventListener('blur', releaseAll);

    canvas.addEventListener('click', () => canvas.focus());
    canvas.focus();
}

// --- File I/O ------------------------------------------------------------

const SD_ROOT = '/sd';
const BUNDLE_ROOT = '/bundle';
const POPULATED_KEY = 'picomite.sd.populated';

// ---- Persistent storage: /sd/ mounted on IndexedDB via IDBFS -----------
//
// Files the user SAVE's from BASIC survive page reloads. First load
// copies the bundled demos from /bundle/ (read-only MEMFS populated by
// --preload-file) into /sd/ and sets a localStorage flag so we don't
// repeatedly clobber user edits. Later versions that ship new demos
// appear only via the "Reset storage" button, which drops the whole
// IndexedDB store and runs the populate pass again.
//
// The flag lives in localStorage (not as a dotfile in /sd/) so the
// MMBasic FILES command never sees bookkeeping state.

function syncfsPromise(instance, populate) {
    return new Promise((resolve, reject) => {
        instance.FS.syncfs(populate, (err) => err ? reject(err) : resolve());
    });
}

function populateFromBundle(instance) {
    let files;
    try {
        files = instance.FS.readdir(BUNDLE_ROOT).filter(n => n !== '.' && n !== '..');
    } catch (_) {
        return 0;
    }
    for (const name of files) {
        try {
            const data = instance.FS.readFile(`${BUNDLE_ROOT}/${name}`);
            instance.FS.writeFile(`${SD_ROOT}/${name}`, data);
        } catch (e) {
            console.warn('bundle copy skipped:', name, e);
        }
    }
    return files.length;
}

async function mountPersistentSd(instance) {
    try { instance.FS.mkdir(SD_ROOT); } catch (_) { /* already exists */ }
    instance.FS.mount(instance.FS.filesystems.IDBFS, {}, SD_ROOT);
    // Pull existing IDB contents into the in-memory view.
    await syncfsPromise(instance, true);

    // Populate from bundle if (a) we've never populated before, OR
    // (b) /sd/ is empty (e.g. a stale localStorage flag from a previous
    // dev session or crash left us in an inconsistent state). Honour
    // the flag only when there's actual content — respects the user's
    // intent to start over after a KILL-everything, but self-heals
    // when the flag is bogus. "Reset /sd/" still forces a repopulate.
    const populated = localStorage.getItem(POPULATED_KEY) === '1';
    const sdEmpty = (() => {
        try {
            return instance.FS.readdir(SD_ROOT)
                .filter(n => n !== '.' && n !== '..').length === 0;
        } catch (_) { return true; }
    })();
    if (!populated || sdEmpty) {
        const n = populateFromBundle(instance);
        if (n > 0) {
            await syncfsPromise(instance, false);
            localStorage.setItem(POPULATED_KEY, '1');
            console.info('[picomite] populated /sd/ from bundle:', n, 'files');
        }
    }
}

async function resetSd(instance) {
    // Wipe every entry under /sd/, persist the empty state to IDB, then
    // re-populate from the bundle.
    try {
        const names = instance.FS.readdir(SD_ROOT).filter(n => n !== '.' && n !== '..');
        for (const n of names) {
            try { instance.FS.unlink(`${SD_ROOT}/${n}`); } catch (_) {}
        }
    } catch (e) {
        console.warn('reset: readdir failed', e);
    }
    const n = populateFromBundle(instance);
    await syncfsPromise(instance, false);
    localStorage.setItem(POPULATED_KEY, '1');
    return n;
}

// Dirty-aware background flush. Any write on the C side (SAVE, KILL,
// EDIT's F1-save path, drag-drop import) lives in the in-memory IDBFS
// view until syncfs(false) commits it to IndexedDB. syncfs is
// synchronous on the main thread: it walks every file, serialises
// changes, and runs an IndexedDB transaction — 5-30 ms of hitch on
// top of whatever frame work is happening. Running it on a 2 s
// interval regardless of whether anything changed was the main
// source of visible stutter during FASTGFX games like pico_blocks.
//
// Fix: only flush when the FS.trackingDelegate hook says /sd/ has
// been written, and schedule the flush via requestIdleCallback so it
// runs between rAFs instead of stepping on one. visibilitychange /
// beforeunload still force-flush unconditionally — they're the "we
// may never get another chance" boundaries.
let sdDirty = false;
let flushTimer = 0;

function installFsDirtyTracker(instance) {
    // FS.trackingDelegate fires for any FS mutation. We only care
    // about paths under /sd/ — everything else is either bundle read
    // (/bundle/) or libc scratch (/tmp/).
    const isSd = (p) => typeof p === 'string' && p.startsWith(SD_ROOT + '/');
    const delegate = instance.FS.trackingDelegate || {};
    delegate.onWriteToFile = (path) => { if (isSd(path)) sdDirty = true; };
    delegate.onDeletePath  = (path) => { if (isSd(path)) sdDirty = true; };
    delegate.onMovePath    = (oldPath, newPath) => {
        if (isSd(oldPath) || isSd(newPath)) sdDirty = true;
    };
    // onOpenFile fires for reads too, so we don't set dirty here;
    // onWriteToFile covers actual writes including SAVE / F1-save.
    instance.FS.trackingDelegate = delegate;
}

function scheduleFlush(instance) {
    sdDirty = true;  // explicit writes (drag-drop, upload, reset)
    if (flushTimer) return;
    const run = async () => {
        flushTimer = 0;
        if (!sdDirty) return;
        sdDirty = false;
        try { await syncfsPromise(instance, false); }
        catch (e) {
            console.warn('syncfs failed:', e);
            sdDirty = true;  // retry on next tick
        }
    };
    // requestIdleCallback gives us a timeslice when the main thread is
    // genuinely idle — never competes with a running FASTGFX frame.
    // Fallback to setTimeout for Safari (no rIC as of 17.x).
    if (typeof requestIdleCallback === 'function') {
        flushTimer = requestIdleCallback(run, { timeout: 4000 });
    } else {
        flushTimer = setTimeout(run, 1500);
    }
}

function flashHint(msg, durationMs = 2500) {
    hintEl.textContent = msg;
    setTimeout(() => {
        hintEl.textContent = 'Drop .bas files anywhere on the page to import.';
    }, durationMs);
}

async function importFile(instance, file) {
    const buf = new Uint8Array(await file.arrayBuffer());
    // Strip any path; keep just the base name. MEMFS under /sd is flat.
    const name = file.name.split(/[/\\]/).pop();
    try {
        instance.FS.writeFile(`${SD_ROOT}/${name}`, buf);
        return name;
    } catch (e) {
        console.error('import failed for', name, e);
        return null;
    }
}

async function importFiles(instance, files) {
    const imported = [];
    for (const f of Array.from(files)) {
        const name = await importFile(instance, f);
        if (name) imported.push(name);
    }
    if (imported.length === 1) {
        flashHint(`Loaded ${imported[0]} — type FILES to list.`, 3500);
    } else if (imported.length > 1) {
        flashHint(`Loaded ${imported.length} files — type FILES to list.`, 3500);
    }
    return imported;
}

function listSd(instance) {
    try {
        return instance.FS.readdir(SD_ROOT)
            .filter(n => n !== '.' && n !== '..');
    } catch (e) {
        return [];
    }
}

function isReadableFile(instance, path) {
    try {
        const stat = instance.FS.stat(path);
        return (stat.mode & 0xF000) === 0x8000;  // S_IFREG
    } catch (e) {
        return false;
    }
}

// Minimal store-only ZIP writer. Good enough for a handful of tiny
// .bas files — no compression, no ZIP64, no encryption. Saves pulling
// in a 30 KB dep like fflate when the payload is a few KB.
function buildZip(entries) {
    const enc = new TextEncoder();
    const parts = [];
    const central = [];
    let offset = 0;

    const crc32Table = buildCrcTable();
    function crc32(bytes) {
        let c = ~0 >>> 0;
        for (let i = 0; i < bytes.length; i++) {
            c = crc32Table[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8);
        }
        return (~c) >>> 0;
    }

    for (const { name, data } of entries) {
        const nameBytes = enc.encode(name);
        const crc = crc32(data);
        const localHeader = new ArrayBuffer(30 + nameBytes.length);
        const lh = new DataView(localHeader);
        lh.setUint32(0,  0x04034b50, true);   // local file header signature
        lh.setUint16(4,  20, true);            // version needed
        lh.setUint16(6,  0, true);             // flags
        lh.setUint16(8,  0, true);             // method = store
        lh.setUint16(10, 0, true);             // time
        lh.setUint16(12, 0, true);             // date
        lh.setUint32(14, crc, true);
        lh.setUint32(18, data.length, true);
        lh.setUint32(22, data.length, true);
        lh.setUint16(26, nameBytes.length, true);
        lh.setUint16(28, 0, true);
        new Uint8Array(localHeader, 30).set(nameBytes);
        parts.push(new Uint8Array(localHeader));
        parts.push(data);

        const ch = new ArrayBuffer(46 + nameBytes.length);
        const chv = new DataView(ch);
        chv.setUint32(0,  0x02014b50, true);
        chv.setUint16(4,  20, true);
        chv.setUint16(6,  20, true);
        chv.setUint16(8,  0, true);
        chv.setUint16(10, 0, true);
        chv.setUint16(12, 0, true);
        chv.setUint16(14, 0, true);
        chv.setUint32(16, crc, true);
        chv.setUint32(20, data.length, true);
        chv.setUint32(24, data.length, true);
        chv.setUint16(28, nameBytes.length, true);
        chv.setUint16(30, 0, true);
        chv.setUint16(32, 0, true);
        chv.setUint16(34, 0, true);
        chv.setUint16(36, 0, true);
        chv.setUint32(38, 0, true);
        chv.setUint32(42, offset, true);
        new Uint8Array(ch, 46).set(nameBytes);
        central.push(new Uint8Array(ch));

        offset += 30 + nameBytes.length + data.length;
    }

    const centralSize = central.reduce((n, b) => n + b.length, 0);
    const eocd = new ArrayBuffer(22);
    const ev = new DataView(eocd);
    ev.setUint32(0,  0x06054b50, true);
    ev.setUint16(4,  0, true);
    ev.setUint16(6,  0, true);
    ev.setUint16(8,  entries.length, true);
    ev.setUint16(10, entries.length, true);
    ev.setUint32(12, centralSize, true);
    ev.setUint32(16, offset, true);
    ev.setUint16(20, 0, true);

    return new Blob([...parts, ...central, new Uint8Array(eocd)],
                    { type: 'application/zip' });
}

function buildCrcTable() {
    const table = new Uint32Array(256);
    for (let i = 0; i < 256; i++) {
        let c = i;
        for (let k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
        table[i] = c >>> 0;
    }
    return table;
}

function triggerDownload(blob, filename) {
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    a.remove();
    // Release the object URL on the next tick so the download has
    // actually started.
    setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function wireFileIO(instance) {
    // Install the FS hook so BASIC-side writes (SAVE, KILL, EDIT's
    // F1-save path) flip the dirty flag, then rely on scheduleFlush
    // to pick them up during idle time. Without this tracker, the old
    // code ran syncfs every 2 s regardless — visible hitch during
    // FASTGFX games.
    installFsDirtyTracker(instance);

    // visibilitychange / beforeunload still force-flush unconditionally:
    // these are the "last chance before the tab goes away" boundaries
    // where we'd rather pay a 5-30 ms hitch than lose a SAVE.
    const flushNow = () => syncfsPromise(instance, false).catch((e) => {
        console.warn('syncfs failed:', e);
    });
    document.addEventListener('visibilitychange', () => {
        if (document.visibilityState === 'hidden') flushNow();
    });
    window.addEventListener('beforeunload', () => { flushNow(); });

    // Poll the dirty flag every 2 s; if something changed on the FS
    // since the last check, scheduleFlush queues an idle-time syncfs.
    // Idle games with no FS writes pay nothing — the polled flag
    // stays false and the timer body returns in microseconds.
    setInterval(() => {
        if (sdDirty) scheduleFlush(instance);
    }, 2000);

    // Upload button (for mobile / touch where drag-drop is awkward).
    uploadInput.addEventListener('change', async (e) => {
        const files = e.target.files;
        if (!files || !files.length) return;
        await importFiles(instance, files);
        scheduleFlush(instance);
        uploadInput.value = '';  // allow re-picking the same file
    });

    // Drag-drop on the whole window.
    let dragDepth = 0;
    window.addEventListener('dragenter', (e) => {
        if (!e.dataTransfer || !e.dataTransfer.types.includes('Files')) return;
        dragDepth++;
        dropOverlay.hidden = false;
        e.preventDefault();
    });
    window.addEventListener('dragleave', (e) => {
        if (--dragDepth <= 0) {
            dragDepth = 0;
            dropOverlay.hidden = true;
        }
        e.preventDefault();
    });
    window.addEventListener('dragover', (e) => {
        if (e.dataTransfer && e.dataTransfer.types.includes('Files')) e.preventDefault();
    });
    window.addEventListener('drop', async (e) => {
        dragDepth = 0;
        dropOverlay.hidden = true;
        if (!e.dataTransfer || !e.dataTransfer.files.length) return;
        e.preventDefault();
        await importFiles(instance, e.dataTransfer.files);
        scheduleFlush(instance);
        canvas.focus();
    });

    // Reset /sd/ — wipes IDBFS contents and re-populates from /bundle/.
    resetBtn.addEventListener('click', async () => {
        if (!confirm('Wipe all files in /sd/ and restore the bundled demos? Any SAVEd programs you haven\'t downloaded will be lost.')) return;
        try {
            const n = await resetSd(instance);
            flashHint(`Reset /sd/ — ${n} demos restored. Type FILES.`, 3500);
        } catch (e) {
            flashHint('Reset failed: ' + (e?.message || e));
        }
    });

    // Download all: pack everything in /sd/ into a ZIP.
    downloadBtn.addEventListener('click', () => {
        const names = listSd(instance);
        const entries = [];
        for (const n of names) {
            const path = `${SD_ROOT}/${n}`;
            if (!isReadableFile(instance, path)) continue;
            try {
                const data = instance.FS.readFile(path);
                entries.push({ name: n, data });
            } catch (e) {
                console.warn('skip', path, e);
            }
        }
        if (!entries.length) {
            flashHint('No files in /sd/ to download.');
            return;
        }
        const zip = buildZip(entries);
        const date = new Date().toISOString().slice(0, 10);
        triggerDownload(zip, `mmbasic-web-${date}.zip`);
        flashHint(`Downloaded ${entries.length} files.`);
    });
}

// --- Boot ---------------------------------------------------------------

try {
    const resolution = pickResolution();
    applyResolutionSelect(resolution.label);
    resolutionSelect.addEventListener('change', () => {
        reloadWithResolution(resolutionSelect.value);
    });

    const memoryBytes = pickMemory();
    applyMemorySelect(memoryBytes);
    memorySelect.addEventListener('change', () => {
        reloadWithMemory(parseInt(memorySelect.value, 10));
    });

    let slowdownUs = pickSlowdown();
    applySlowdownInputs(slowdownUs);

    // Dialog open/close.
    openConfigBtn.addEventListener('click', () => {
        if (typeof configDialog.showModal === 'function') {
            configDialog.showModal();
        } else {
            configDialog.setAttribute('open', '');
        }
    });
    configCloseBtn.addEventListener('click', () => configDialog.close());

    const instance = await Module({
        print:    (line) => console.log('[picomite]', line),
        printErr: (line) => console.warn('[picomite]', line),
    });

    // Dev/test hook: expose the WASM instance so headless smoke tests
    // can poke at HEAPU32, _wasm_framebuffer_generation, FS.readFile,
    // etc. Not used by the page UI itself.
    window.picomite = { instance };

    // Wire live slowdown updates now that the instance exists. Both
    // sides in µs, matching the native --slowdown flag. The WASM-side
    // host_sim_apply_slowdown accumulates µs and only sleeps on whole-ms
    // boundaries, so sub-ms values are meaningful here.
    applySlowdownLive = (us) => {
        slowdownUs = us;
        try { localStorage.setItem(SLOW_KEY, String(us)); } catch (_) {}
        instance._wasm_set_slowdown_us(us);
    };
    applySlowdownLive(slowdownUs);

    const clampSlowInput = (raw) => {
        let n = parseInt(raw, 10);
        if (!Number.isFinite(n) || n < 0) n = 0;
        if (n > SLOW_MAX_US) n = SLOW_MAX_US;
        return n;
    };
    slowdownRange.addEventListener('input', () => {
        const n = clampSlowInput(slowdownRange.value);
        slowdownNumber.value = String(n);
        applySlowdownLive(n);
    });
    slowdownNumber.addEventListener('input', () => {
        const n = clampSlowInput(slowdownNumber.value);
        slowdownRange.value = String(Math.min(n, parseInt(slowdownRange.max, 10)));
        applySlowdownLive(n);
    });

    // Both setters MUST run before wasm_boot — InitBasic inside
    // wasm_boot reads heap_memory_size to size the mmap bitmap, and
    // host_runtime_begin allocates the framebuffer from
    // host_fb_width/height.
    instance._wasm_set_framebuffer_size(resolution.w, resolution.h);
    instance._wasm_set_heap_size(memoryBytes);

    // Mount /sd/ on IDBFS so files the user SAVEs survive reloads.
    // Must happen before wasm_boot — host_fs_shims.c reads from /sd/
    // as soon as InitBasic / the REPL gets going.
    try {
        await mountPersistentSd(instance);
    } catch (e) {
        console.warn('IDBFS mount failed, falling back to MEMFS:', e);
    }

    fbWidth  = instance._wasm_framebuffer_width();
    fbHeight = instance._wasm_framebuffer_height();
    fbPtr    = instance._wasm_framebuffer_ptr();

    canvas.width  = fbWidth;
    canvas.height = fbHeight;
    gl.viewport(0, 0, fbWidth, fbHeight);
    fitCanvas();
    window.addEventListener('resize', fitCanvas);

    wireKeyboard(instance);
    wireFileIO(instance);
    startRenderLoop(instance);

    const demos = listSd(instance);
    const memLabel = memoryBytes >= 1024 * 1024
        ? `${(memoryBytes / (1024 * 1024)).toFixed(memoryBytes % (1024 * 1024) ? 1 : 0)} MB`
        : `${Math.round(memoryBytes / 1024)} KB`;
    const parts = [`${fbWidth}×${fbHeight}`, `${memLabel} heap`];
    if (slowdownUs > 0) parts.push(`slowdown ${slowdownUs} µs`);
    if (demos.length) parts.push(`${demos.length} demos in /sd/`);
    setStatus(`Ready — ${parts.join(', ')}. Type FILES.`);

    await instance.ccall('wasm_boot', null, [], [], { async: true });
} catch (err) {
    setStatus('Fatal: ' + (err?.message || err), true);
    console.error(err);
}
