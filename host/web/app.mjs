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
const dropOverlay = document.getElementById('drop-overlay');
const hintEl = document.getElementById('hint');
const ctx = canvas.getContext('2d', { alpha: false });

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
    // Upload button (for mobile / touch where drag-drop is awkward).
    uploadInput.addEventListener('change', async (e) => {
        const files = e.target.files;
        if (!files || !files.length) return;
        await importFiles(instance, files);
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
        canvas.focus();
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
    const instance = await Module({
        print:    (line) => console.log('[picomite]', line),
        printErr: (line) => console.warn('[picomite]', line),
    });

    fbWidth  = instance._wasm_framebuffer_width();
    fbHeight = instance._wasm_framebuffer_height();
    fbPtr    = instance._wasm_framebuffer_ptr();

    canvas.width  = fbWidth;
    canvas.height = fbHeight;
    imageData = ctx.createImageData(fbWidth, fbHeight);

    wireKeyboard(instance);
    wireFileIO(instance);
    startRenderLoop(instance);

    const demos = listSd(instance);
    setStatus(
        demos.length
            ? `Ready (${fbWidth}x${fbHeight}) — ${demos.length} demos in /sd/. Type FILES.`
            : `Ready (${fbWidth}x${fbHeight}).`
    );

    await instance.ccall('wasm_boot', null, [], [], { async: true });
} catch (err) {
    setStatus('Fatal: ' + (err?.message || err), true);
    console.error(err);
}
