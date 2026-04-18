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
const resolutionSelect = document.getElementById('resolution');
const memorySelect = document.getElementById('memory');
const dropOverlay = document.getElementById('drop-overlay');
const hintEl = document.getElementById('hint');
const ctx = canvas.getContext('2d', { alpha: false });

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

let fbPtr = 0, fbWidth = 0, fbHeight = 0, imageData = null;

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

function blitFrame(instance) {
    if (!fbPtr) return;
    const src = instance.HEAPU32.subarray(fbPtr >>> 2, (fbPtr >>> 2) + fbWidth * fbHeight);
    const dst = imageData.data;
    for (let i = 0, j = 0; i < src.length; i++, j += 4) {
        const px = src[i];
        dst[j]     = (px >>> 16) & 0xFF;
        dst[j + 1] = (px >>> 8)  & 0xFF;
        dst[j + 2] =  px         & 0xFF;
        dst[j + 3] = 0xFF;
    }
    ctx.putImageData(imageData, 0, 0);
}

function startRenderLoop(instance) {
    const loop = () => {
        try { blitFrame(instance); }
        catch (e) { setStatus('Render error: ' + e.message, true); return; }
        requestAnimationFrame(loop);
    };
    requestAnimationFrame(loop);
}

// --- Keyboard ------------------------------------------------------------

function wireKeyboard(instance) {
    const pushKey = instance.cwrap('wasm_push_key', null, ['number']);

    canvas.addEventListener('keydown', (event) => {
        const code = mapKeyEvent(event);
        if (code < 0) return;
        event.preventDefault();
        pushKey(code);
    });

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
    if (localStorage.getItem(POPULATED_KEY) !== '1') {
        const n = populateFromBundle(instance);
        await syncfsPromise(instance, false);
        localStorage.setItem(POPULATED_KEY, '1');
        console.info('[picomite] populated /sd/ from bundle:', n, 'files');
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

// Debounced background flush. Any write on the C side (SAVE, KILL,
// EDIT's F1-save path) lives in the in-memory IDBFS view until
// syncfs(false) commits it to IndexedDB. Flushing every few seconds
// plus on tab-hide / beforeunload covers the common cases.
let flushTimer = 0;
function scheduleFlush(instance) {
    if (flushTimer) return;
    flushTimer = setTimeout(async () => {
        flushTimer = 0;
        try { await syncfsPromise(instance, false); }
        catch (e) { console.warn('syncfs failed:', e); }
    }, 1500);
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
    // Flush /sd/ to IndexedDB when the tab goes away or every few
    // seconds after a change. Drag-drop, upload, and reset all trigger
    // a scheduleFlush directly; regular SAVE from BASIC just leaves the
    // MEMFS view dirty, so we also flush on visibilitychange /
    // beforeunload as a belt-and-braces measure.
    const flushNow = () => syncfsPromise(instance, false).catch((e) => {
        console.warn('syncfs failed:', e);
    });
    document.addEventListener('visibilitychange', () => {
        if (document.visibilityState === 'hidden') flushNow();
    });
    window.addEventListener('beforeunload', () => { flushNow(); });
    // Cheap periodic flush covers BASIC-side writes (SAVE, KILL,
    // EDIT's save-on-F1). 2 s is frequent enough that a quick tab
    // close rarely loses work, sparse enough that idle pages cost
    // almost nothing.
    setInterval(flushNow, 2000);

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

    const instance = await Module({
        print:    (line) => console.log('[picomite]', line),
        printErr: (line) => console.warn('[picomite]', line),
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
    imageData = ctx.createImageData(fbWidth, fbHeight);
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
    if (demos.length) parts.push(`${demos.length} demos in /sd/`);
    setStatus(`Ready — ${parts.join(', ')}. Type FILES.`);

    await instance.ccall('wasm_boot', null, [], [], { async: true });
} catch (err) {
    setStatus('Fatal: ' + (err?.message || err), true);
    console.error(err);
}
