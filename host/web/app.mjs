// MMBasic Web loader.
//
// Boots a dedicated Web Worker (worker.mjs) that owns the MMBasic
// runtime. The main thread handles canvas rendering, keyboard input,
// audio dispatch, and the file-IO UI (drag-drop, download, reset).
// The worker's wasm linear memory is a SharedArrayBuffer so this
// thread's rAF blit reads the framebuffer zero-copy — no postMessage
// per frame.
//
// Files live under /sd/ in emscripten's IDBFS mount (handled inside
// the worker). Drag-drop + upload write fresh files; download-all
// packages /sd/ as a ZIP. The Reset button wipes /sd/ and re-populates
// from the bundled /bundle/ snapshot.

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
const filesListEl = document.getElementById('files-list');

if (typeof SharedArrayBuffer === 'undefined') {
    statusEl.textContent = 'SharedArrayBuffer not available — page must be served with COOP/COEP headers (see serve.py).';
    statusEl.classList.add('error');
    throw new Error('SAB unavailable');
}

// ---- WebGL setup --------------------------------------------------------
//
// WebGL2 commits go straight to the GPU compositor. The shader below
// swizzles R↔B in the fragment stage because host_framebuffer stores
// pixels as (R<<16)|(G<<8)|B in a uint32 plane, which when uploaded
// as RGBA bytes lands as (B,G,R,0) per texel.

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
    statusEl.textContent = 'WebGL2 not available in this browser.';
    throw new Error('WebGL2 required');
}
gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);

const VERT_SRC = `#version 300 es
in vec2 a_pos;
out vec2 v_uv;
void main() { v_uv = a_pos * 0.5 + 0.5; gl_Position = vec4(a_pos, 0.0, 1.0); }`;
const FRAG_SRC = `#version 300 es
precision mediump float;
in vec2 v_uv;
uniform sampler2D u_tex;
out vec4 outColor;
void main() { vec4 c = texture(u_tex, v_uv); outColor = vec4(c.b, c.g, c.r, 1.0); }`;

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

const glTex = gl.createTexture();
gl.activeTexture(gl.TEXTURE0);
gl.bindTexture(gl.TEXTURE_2D, glTex);
gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

// ---- Resolution / memory / slowdown selection ---------------------------
//
// Framebuffer size must be set before wasm_boot (allocation happens
// during host_runtime_begin in the worker). Resolution changes trigger
// a full page reload with ?res=WxH — re-initialising the interpreter
// mid-session is fragile because of setjmp/longjmp state.

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

const DEFAULT_MEM = 2097152;
const MEM_KEY = 'picomite.mem';
const MEM_MIN = 32 * 1024;
const MEM_MAX = 8 * 1024 * 1024;

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

// Slowdown: applies live. Worker will accept this via the init cfg or a
// live setter; for now it's set once at boot. Per-statement sub-ms
// accumulation happens C-side in host_sim_apply_slowdown.
const DEFAULT_SLOWDOWN_US = 0;
const SLOW_KEY = 'picomite.slowdown.us';
const SLOW_MAX_US = 100000;

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

// ---- MMBasic key-code mapping -------------------------------------------

const SPECIAL_KEYS = {
    Enter: 0x0D, Tab: 0x09, Backspace: 0x08, Delete: 0x7F, Escape: 0x1B,
    ArrowUp: 0x80, ArrowDown: 0x81, ArrowLeft: 0x82, ArrowRight: 0x83,
    Insert: 0x84, Home: 0x86, End: 0x87, PageUp: 0x88, PageDown: 0x89,
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

// ---- Framebuffer blit ---------------------------------------------------

const DESIRED_SCALE = 2;
const VIEWPORT_CHROME_PX = 140;

let fbWidth = 0, fbHeight = 0, fbPtr = 0;
let fbGenerationIdx = 0;        // HEAPU32 index of the generation counter
let memoryBytes = null;         // Uint8Array view over the worker's shared heap
let memoryU32 = null;           // Uint32Array view, same buffer
let memoryI32 = null;           // Int32Array view, same buffer
let fbTexAllocated = false;

// Direct-write indices into the wasm key ring (filled in when the
// worker posts 'ready'). Main thread pushes keys via Atomics.store
// without a postMessage round-trip.
let keyRingI32Base = 0, keyHeadIdx = 0, keyTailIdx = 0, keyRingMask = 0;

function fitCanvas() {
    if (!fbWidth || !fbHeight) return;
    const maxW = Math.max(160, Math.floor(window.innerWidth * 0.92));
    const maxH = Math.max(160, window.innerHeight - VIEWPORT_CHROME_PX);
    const fit = Math.min(maxW / fbWidth, maxH / fbHeight);
    const scale = Math.min(DESIRED_SCALE, fit);
    canvas.style.width  = `${Math.floor(fbWidth  * scale)}px`;
    canvas.style.height = `${Math.floor(fbHeight * scale)}px`;
}

function blitFrame() {
    const bytes = new Uint8Array(memoryBytes.buffer, fbPtr, fbWidth * fbHeight * 4);
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

function startRenderLoop() {
    let lastGen = 0xFFFFFFFF;
    const loop = () => {
        try {
            const gen = memoryU32[fbGenerationIdx];
            if (gen !== lastGen) {
                lastGen = gen;
                blitFrame();
            }
        } catch (e) {
            setStatus('Render error: ' + e.message, true);
            return;
        }
        requestAnimationFrame(loop);
    };
    requestAnimationFrame(loop);
}

// ---- Worker + RPC plumbing ----------------------------------------------

const worker = new Worker(new URL('./worker.mjs', import.meta.url), { type: 'module' });

const pendingReplies = new Map();  // reqId -> {resolve, reject}
let nextReqId = 1;

function workerRpc(type, extra = {}) {
    const reqId = nextReqId++;
    return new Promise((resolve, reject) => {
        pendingReplies.set(reqId, { resolve, reject });
        worker.postMessage({ type, reqId, ...extra });
    });
}

worker.onerror = (e) => {
    setStatus('Worker error: ' + (e.message || e), true);
    console.error(e);
};

worker.onmessage = (e) => {
    const m = e.data;
    if (!m || !m.type) return;

    if (m.type === 'log') {
        console[m.level === 'warn' ? 'warn' : 'log']('[picomite]', m.line);
        return;
    }

    if (m.type === 'audio') {
        const api = window.picomiteAudio;
        if (api && typeof api[m.op] === 'function') api[m.op](...(m.args || []));
        return;
    }

    if (m.type === 'ready') {
        memoryBytes = new Uint8Array(m.memoryBuffer);
        memoryU32   = new Uint32Array(m.memoryBuffer);
        memoryI32   = new Int32Array(m.memoryBuffer);
        fbPtr       = m.fbPtr;
        fbWidth     = m.fbWidth;
        fbHeight    = m.fbHeight;
        fbGenerationIdx = m.fbGenerationPtr >>> 2;
        keyRingI32Base  = m.keyRingPtr     >>> 2;
        keyHeadIdx      = m.keyRingHeadPtr >>> 2;
        keyTailIdx      = m.keyRingTailPtr >>> 2;
        keyRingMask     = m.keyRingSize - 1;
        onWorkerReady();
        return;
    }

    // Everything below is a reqId-tagged reply to workerRpc().
    const pending = pendingReplies.get(m.reqId);
    if (!pending) return;
    pendingReplies.delete(m.reqId);
    if (m.error) pending.reject(new Error(m.error));
    else pending.resolve(m);
};

// FS round-trip helpers. All operations are fire-and-forget OR promise-
// based — the worker owns the single instance.FS and mediates access.

function fsWrite(path, bytes) {
    // Transfer the buffer so we don't copy the file bytes across threads.
    const buf = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
    worker.postMessage({ type: 'fs-write', path, data: buf }, [buf]);
}

async function fsList(dir) {
    const m = await workerRpc('fs-list', { dir });
    return m.names || [];
}

async function fsRead(path) {
    const m = await workerRpc('fs-read', { path });
    if (!m.data) throw new Error(m.error || 'fs-read failed');
    return new Uint8Array(m.data);
}

async function fsReset() {
    const m = await workerRpc('fs-reset');
    return m.count || 0;
}

function fsUnlink(path) {
    worker.postMessage({ type: 'fs-unlink', path });
}

// ---- File IO UI ---------------------------------------------------------

const SD_ROOT = '/sd';

// Transient status-line messages. The status bar normally shows the
// ready/capacity line; a transient message replaces it for a few
// seconds after a user action (import, delete, reset) and then
// restores the persistent text.
let persistentStatus = '';
let flashTimer = 0;
function setStatus(text, isError = false) {
    persistentStatus = text;
    statusEl.textContent = text;
    statusEl.classList.toggle('error', isError);
}
function flash(text, durationMs = 2500) {
    statusEl.textContent = text;
    statusEl.classList.remove('error');
    clearTimeout(flashTimer);
    flashTimer = setTimeout(() => {
        statusEl.textContent = persistentStatus;
    }, durationMs);
}

// File-list rendering. Refreshed after every import / delete / reset;
// also polled every 3 s so BASIC-side SAVE shows up without needing to
// hook the worker's FS tracker into a message stream.
let lastFilesSig = '';
async function refreshFilesList() {
    let names;
    try { names = await fsList(SD_ROOT); }
    catch { names = []; }
    names.sort((a, b) => a.localeCompare(b));
    const sig = names.join('\u0001');
    if (sig === lastFilesSig) return names;
    lastFilesSig = sig;

    filesListEl.textContent = '';
    for (const name of names) {
        const li = document.createElement('li');
        const ico = document.createElement('span');
        ico.className = 'file-icon';
        ico.innerHTML = '<svg class="ico" style="width:14px;height:14px"><use href="#ico-file"/></svg>';
        const label = document.createElement('span');
        label.className = 'file-name';
        label.textContent = name;
        label.title = name;
        const actions = document.createElement('span');
        actions.className = 'file-actions';

        const dl = document.createElement('button');
        dl.type = 'button';
        dl.title = `Download ${name}`;
        dl.innerHTML = '<svg class="ico" style="width:14px;height:14px"><use href="#ico-download"/></svg>';
        dl.addEventListener('click', () => downloadOne(name));
        actions.appendChild(dl);

        const del = document.createElement('button');
        del.type = 'button';
        del.className = 'delete-btn';
        del.title = `Delete ${name}`;
        del.innerHTML = '<svg class="ico" style="width:14px;height:14px"><use href="#ico-trash"/></svg>';
        del.addEventListener('click', () => deleteOne(name));
        actions.appendChild(del);

        li.appendChild(ico);
        li.appendChild(label);
        li.appendChild(actions);
        filesListEl.appendChild(li);
    }
    return names;
}

async function downloadOne(name) {
    try {
        const data = await fsRead(`${SD_ROOT}/${name}`);
        triggerDownload(new Blob([data], { type: 'application/octet-stream' }), name);
    } catch (e) {
        flash(`Download failed: ${e?.message || e}`);
    }
}

async function deleteOne(name) {
    if (!confirm(`Delete ${name}?`)) return;
    fsUnlink(`${SD_ROOT}/${name}`);
    await new Promise((r) => setTimeout(r, 60));  // let worker process
    await refreshFilesList();
    flash(`Deleted ${name}.`);
}

async function importFile(file) {
    const buf = new Uint8Array(await file.arrayBuffer());
    const name = file.name.split(/[/\\]/).pop();  // strip any path
    fsWrite(`${SD_ROOT}/${name}`, buf);
    return name;
}

async function importFiles(files) {
    const imported = [];
    for (const f of Array.from(files)) {
        try { imported.push(await importFile(f)); }
        catch (e) { console.warn('import failed:', f.name, e); }
    }
    // Give the worker a moment to commit the writes before we re-list.
    await new Promise((r) => setTimeout(r, 60));
    await refreshFilesList();
    if (imported.length === 1) flash(`Imported ${imported[0]}.`);
    else if (imported.length > 1) flash(`Imported ${imported.length} files.`);
    return imported;
}

// Minimal store-only ZIP writer. Saves pulling in a 30 KB dep like fflate
// when the payload is a handful of tiny .bas files.
function buildCrcTable() {
    const t = new Uint32Array(256);
    for (let i = 0; i < 256; i++) {
        let c = i;
        for (let k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
        t[i] = c >>> 0;
    }
    return t;
}
function buildZip(entries) {
    const enc = new TextEncoder();
    const parts = [];
    const central = [];
    let offset = 0;
    const crcTable = buildCrcTable();
    const crc32 = (b) => {
        let c = ~0 >>> 0;
        for (let i = 0; i < b.length; i++) c = crcTable[(c ^ b[i]) & 0xFF] ^ (c >>> 8);
        return (~c) >>> 0;
    };
    for (const { name, data } of entries) {
        const nameBytes = enc.encode(name);
        const crc = crc32(data);
        const lhBuf = new ArrayBuffer(30 + nameBytes.length);
        const lh = new DataView(lhBuf);
        lh.setUint32(0, 0x04034b50, true);
        lh.setUint16(4, 20, true);
        lh.setUint16(6, 0, true);
        lh.setUint16(8, 0, true);
        lh.setUint16(10, 0, true);
        lh.setUint16(12, 0, true);
        lh.setUint32(14, crc, true);
        lh.setUint32(18, data.length, true);
        lh.setUint32(22, data.length, true);
        lh.setUint16(26, nameBytes.length, true);
        lh.setUint16(28, 0, true);
        new Uint8Array(lhBuf, 30).set(nameBytes);
        parts.push(new Uint8Array(lhBuf));
        parts.push(data);

        const chBuf = new ArrayBuffer(46 + nameBytes.length);
        const ch = new DataView(chBuf);
        ch.setUint32(0, 0x02014b50, true);
        ch.setUint16(4, 20, true);
        ch.setUint16(6, 20, true);
        ch.setUint16(8, 0, true);
        ch.setUint16(10, 0, true);
        ch.setUint16(12, 0, true);
        ch.setUint16(14, 0, true);
        ch.setUint32(16, crc, true);
        ch.setUint32(20, data.length, true);
        ch.setUint32(24, data.length, true);
        ch.setUint16(28, nameBytes.length, true);
        ch.setUint16(30, 0, true);
        ch.setUint16(32, 0, true);
        ch.setUint16(34, 0, true);
        ch.setUint16(36, 0, true);
        ch.setUint32(38, 0, true);
        ch.setUint32(42, offset, true);
        new Uint8Array(chBuf, 46).set(nameBytes);
        central.push(new Uint8Array(chBuf));

        offset += 30 + nameBytes.length + data.length;
    }
    const centralSize = central.reduce((n, b) => n + b.length, 0);
    const eocdBuf = new ArrayBuffer(22);
    const ev = new DataView(eocdBuf);
    ev.setUint32(0, 0x06054b50, true);
    ev.setUint16(4, 0, true);
    ev.setUint16(6, 0, true);
    ev.setUint16(8, entries.length, true);
    ev.setUint16(10, entries.length, true);
    ev.setUint32(12, centralSize, true);
    ev.setUint32(16, offset, true);
    ev.setUint16(20, 0, true);
    return new Blob([...parts, ...central, new Uint8Array(eocdBuf)],
                    { type: 'application/zip' });
}

function triggerDownload(blob, filename) {
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function wireFileIO() {
    // Upload button (mobile / touch where drag-drop is awkward).
    uploadInput.addEventListener('change', async (e) => {
        const files = e.target.files;
        if (!files || !files.length) return;
        await importFiles(files);
        uploadInput.value = '';
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
        if (--dragDepth <= 0) { dragDepth = 0; dropOverlay.hidden = true; }
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
        await importFiles(e.dataTransfer.files);
        canvas.focus();
    });

    resetBtn.addEventListener('click', async () => {
        if (!confirm('Wipe all files in /sd/ and restore the bundled demos? Any SAVEd programs you haven\'t downloaded will be lost.')) return;
        try {
            const n = await fsReset();
            await refreshFilesList();
            flash(`Reset /sd/ — ${n} demos restored.`);
        } catch (e) {
            flash('Reset failed: ' + (e?.message || e));
        }
    });

    downloadBtn.addEventListener('click', async () => {
        const names = await fsList(SD_ROOT);
        const entries = [];
        for (const n of names) {
            try {
                const data = await fsRead(`${SD_ROOT}/${n}`);
                entries.push({ name: n, data });
            } catch (e) {
                console.warn('skip', n, e);
            }
        }
        if (!entries.length) { flash('No files in /sd/ to download.'); return; }
        const zip = buildZip(entries);
        const date = new Date().toISOString().slice(0, 10);
        triggerDownload(zip, `mmbasic-web-${date}.zip`);
        flash(`Downloaded ${entries.length} files.`);
    });
}

// ---- Keyboard -----------------------------------------------------------
//
// Device defaults: RepeatStart 600 ms, RepeatRate 150 ms (~6.7 Hz).
// Browser auto-repeat fires ~25-35 Hz which is 4-5× too fast for
// action games. We ignore event.repeat and drive a throttled schedule
// per physical key. Passive listeners let Chrome commit compositor
// frames without waiting for us — critical for smooth gameplay.
//
// Keys push directly into the wasm-side key ring through shared
// memory: Atomics.store on the head index is a full memory barrier,
// so by the time the worker observes the new head, the ring slot
// write is also visible. No postMessage hop per key.
const KEY_REPEAT_DELAY_MS    = 400;
const KEY_REPEAT_INTERVAL_MS = 100;

function pushKeyToRing(code) {
    const head = Atomics.load(memoryU32, keyHeadIdx);
    const tail = Atomics.load(memoryU32, keyTailIdx);
    const next = (head + 1) & keyRingMask;
    if (next === tail) return;  // ring full — drop, matches C path
    memoryI32[keyRingI32Base + head] = code;
    Atomics.store(memoryU32, keyHeadIdx, next);
}

function wireKeyboard() {
    const held = new Map();  // event.code -> {delayTimer, intervalTimer, mmCode}
    const releaseAll = () => {
        for (const s of held.values()) {
            if (s.delayTimer)    clearTimeout(s.delayTimer);
            if (s.intervalTimer) clearInterval(s.intervalTimer);
        }
        held.clear();
    };

    canvas.addEventListener('keydown', (event) => {
        const mm = mapKeyEvent(event);
        if (mm < 0) return;
        if (event.repeat) return;
        pushKeyToRing(mm);

        const stale = held.get(event.code);
        if (stale) { clearTimeout(stale.delayTimer); clearInterval(stale.intervalTimer); }

        const s = { delayTimer: 0, intervalTimer: 0, mmCode: mm };
        s.delayTimer = setTimeout(() => {
            s.delayTimer = 0;
            s.intervalTimer = setInterval(() => {
                pushKeyToRing(s.mmCode);
            }, KEY_REPEAT_INTERVAL_MS);
        }, KEY_REPEAT_DELAY_MS);
        held.set(event.code, s);
    }, { passive: true });

    canvas.addEventListener('keyup', (event) => {
        const s = held.get(event.code);
        if (!s) return;
        if (s.delayTimer)    clearTimeout(s.delayTimer);
        if (s.intervalTimer) clearInterval(s.intervalTimer);
        held.delete(event.code);
    }, { passive: true });

    window.addEventListener('blur', releaseAll);
    canvas.addEventListener('blur', releaseAll);
    canvas.addEventListener('click', () => canvas.focus());
    canvas.focus();
}

// ---- Boot ---------------------------------------------------------------

const resolution = pickResolution();
applyResolutionSelect(resolution.label);
resolutionSelect.addEventListener('change', () => reloadWithResolution(resolutionSelect.value));

const memoryBytesCfg = pickMemory();
applyMemorySelect(memoryBytesCfg);
memorySelect.addEventListener('change', () => reloadWithMemory(parseInt(memorySelect.value, 10)));

let slowdownUs = pickSlowdown();
applySlowdownInputs(slowdownUs);

openConfigBtn.addEventListener('click', () => {
    if (typeof configDialog.showModal === 'function') configDialog.showModal();
    else configDialog.setAttribute('open', '');
});
configCloseBtn.addEventListener('click', () => configDialog.close());

const clampSlowInput = (raw) => {
    let n = parseInt(raw, 10);
    if (!Number.isFinite(n) || n < 0) n = 0;
    if (n > SLOW_MAX_US) n = SLOW_MAX_US;
    return n;
};
// Live slowdown: both inputs mirror each other and postMessage the
// new value to the worker, which calls _wasm_set_slowdown_us. Applies
// on the next BASIC statement — no reload.
function applySlowdownLive(us) {
    slowdownUs = us;
    try { localStorage.setItem(SLOW_KEY, String(us)); } catch (_) {}
    worker.postMessage({ type: 'set-slowdown', us });
}
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

// Flush IDBFS on tab-hide / beforeunload — best-effort "last chance"
// boundaries. The worker's periodic flush covers normal operation.
document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'hidden') worker.postMessage({ type: 'fs-syncfs' });
});
window.addEventListener('beforeunload', () => {
    worker.postMessage({ type: 'fs-syncfs' });
});

setStatus('Booting worker…');

worker.postMessage({
    type: 'init',
    cfg: {
        res:       { w: resolution.w, h: resolution.h },
        heap:      memoryBytesCfg,
        slowdown:  slowdownUs,
        persistSd: true,
    },
});

async function onWorkerReady() {
    canvas.width  = fbWidth;
    canvas.height = fbHeight;
    gl.viewport(0, 0, fbWidth, fbHeight);
    fitCanvas();
    window.addEventListener('resize', fitCanvas);

    wireKeyboard();
    wireFileIO();
    startRenderLoop();

    // Dev/test hook — smoke tests poke at this.
    window.picomite = {
        worker, memoryBytes, memoryU32,
        fbPtr, fbWidth, fbHeight, fbGenerationIdx,
        fsList, fsRead, fsReset,
        fsWrite: (path, bytes) => fsWrite(path, bytes),
    };

    const demos = await refreshFilesList();
    // Poll the file list so BASIC-side SAVE shows up without any
    // explicit worker notification. Cheap: a single postMessage
    // round-trip returning a short string array every 3 s.
    setInterval(() => { refreshFilesList().catch(() => {}); }, 3000);

    const memLabel = memoryBytesCfg >= 1024 * 1024
        ? `${(memoryBytesCfg / (1024 * 1024)).toFixed(memoryBytesCfg % (1024 * 1024) ? 1 : 0)} MB`
        : `${Math.round(memoryBytesCfg / 1024)} KB`;
    const parts = [`${fbWidth}×${fbHeight}`, `${memLabel}`];
    if (slowdownUs > 0) parts.push(`slowdown ${slowdownUs} µs`);
    parts.push(`${demos.length} file${demos.length === 1 ? '' : 's'}`);
    setStatus(parts.join(' · '));
}
