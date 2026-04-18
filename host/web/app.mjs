// PicoMite web loader.
//
// Boots the WASM module, wires keyboard → wasm_push_key, and blits the
// host_fb framebuffer onto #screen on every requestAnimationFrame tick.
// Phase 1 does a full-framebuffer putImageData per frame; dirty-rect
// optimisation lands in Phase 6 if profiling demands it.

import Module from './picomite.mjs';

const statusEl = document.getElementById('status');
const canvas = document.getElementById('screen');
const ctx = canvas.getContext('2d', { alpha: false });

function setStatus(text, isError = false) {
    statusEl.textContent = text;
    statusEl.classList.toggle('error', isError);
}

// --- MMBasic key code mapping --------------------------------------------
//
// Values match Hardware_Includes.h (UP=0x80 … PDOWN=0x89, F1=0x91 …
// F12=0x9C). Printable characters pass through as their ASCII code.
// host_runtime.c's MMInkey reads these from the ring via
// host_read_byte_nonblock; no escape-sequence decoding needed because we
// deliver already-decoded codes.

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

// --- Framebuffer blit loop -----------------------------------------------

let fbPtr = 0, fbWidth = 0, fbHeight = 0, imageData = null;

function blitFrame(instance) {
    if (!fbPtr) return;
    // host_framebuffer is uint32_t[] with 0x00RRGGBB in each cell (the
    // low 24 bits carry the colour; high byte is always zero on host).
    // Canvas ImageData wants R,G,B,A bytes — portable conversion that
    // doesn't depend on the WASM heap's endianness.
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

    // Single listener on the canvas (tabindex="0" makes it focusable).
    // Window-level fallback isn't needed because we focus the canvas at
    // boot and on click.
    canvas.addEventListener('keydown', (event) => {
        const code = mapKeyEvent(event);
        if (code < 0) return;
        event.preventDefault();
        pushKey(code);
    });

    canvas.addEventListener('click', () => canvas.focus());
    canvas.focus();
}

// --- Boot ---------------------------------------------------------------

try {
    const instance = await Module({
        // Route libc stdout/stderr into the console — only for diagnostics.
        // Console output visible to the user is rendered into the
        // framebuffer by gfx_console_shared.c, not here.
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
    startRenderLoop(instance);

    setStatus(`PicoMite WASM ready (${fbWidth}x${fbHeight}) — click the screen, then type.`);

    // wasm_boot does not return — MMBasic_RunPromptLoop holds the event
    // loop until the user navigates away, yielding back to JS via
    // emscripten_sleep (ASYNCIFY) on every blocking read.
    await instance.ccall('wasm_boot', null, [], [], { async: true });
} catch (err) {
    setStatus('Fatal: ' + (err?.message || err), true);
    console.error(err);
}
