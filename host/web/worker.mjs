// worker.mjs — hosts the MMBasic runtime on a dedicated thread.
//
// Motivation (see docs/web-host-plan.md Phase 8): Chrome on macOS
// halves rAF cadence when continuous keyboard input contends with
// setTimeout-driven ASYNCIFY sleeps on the main thread. Moving the
// interpreter to a worker means its event loop is independent of
// main-thread input scheduling. Main thread does canvas + keyboard +
// audio + FS UI only; this worker owns the wasm runtime.
//
// Shared memory: the wasm build uses `-pthread`, so wasm's linear
// memory is a SharedArrayBuffer. After boot we postMessage the buffer
// + framebuffer pointer back to main thread so its rAF loop can blit
// the same bytes the interpreter is writing. Zero-copy across threads.
//
// Communication:
//   inbound  'init'    — config (resolution, heap size, slowdown)
//   inbound  'key'     — keydown from main: code = MMBasic key value
//   inbound  'fs-write' — main-thread file import: path + bytes
//   inbound  'fs-reset' — wipe /sd/ and re-populate from /bundle/
//   inbound  'fs-list'  — return /sd/ contents (for download)
//   inbound  'fs-read'  — return a single file's bytes
//   outbound 'ready'    — wasm ready; shared memory + fb info
//   outbound 'log'      — print / printErr passthrough
//   outbound 'audio'    — wasm called a PLAY primitive (forwarded to
//                         Web Audio on main thread)
//   outbound 'fs-list-result', 'fs-read-result' — responses

import Module from './picomite.mjs';

let instance = null;

async function initInstance(cfg) {
    instance = await Module({
        print:    (line) => self.postMessage({ type: 'log', level: 'log',  line }),
        printErr: (line) => self.postMessage({ type: 'log', level: 'warn', line }),
    });

    if (cfg.res)      instance._wasm_set_framebuffer_size(cfg.res.w, cfg.res.h);
    if (cfg.heap)     instance._wasm_set_heap_size(cfg.heap);
    if (cfg.slowdown) instance._wasm_set_slowdown_us(cfg.slowdown);

    // Copy the preloaded demos from /bundle/ into /sd/ so FILES / RUN
    // work without any further setup. The main-thread app.mjs normally
    // mounts IDBFS at /sd/ and does this on first-load; the worker
    // test path is transient by design — no persistence — so we just
    // copy each boot.
    try {
        instance.FS.mkdir('/sd');
    } catch (_) { /* already exists */ }
    try {
        for (const name of instance.FS.readdir('/bundle')) {
            if (name === '.' || name === '..') continue;
            try {
                const data = instance.FS.readFile('/bundle/' + name);
                instance.FS.writeFile('/sd/' + name, data);
            } catch (e) {
                self.postMessage({ type: 'log', level: 'warn',
                    line: 'bundle copy skipped ' + name + ': ' + e });
            }
        }
    } catch (e) {
        self.postMessage({ type: 'log', level: 'warn',
            line: 'bundle readdir failed: ' + e });
    }

    self.postMessage({
        type: 'ready',
        memoryBuffer: instance.HEAPU8.buffer,  // SharedArrayBuffer (with -pthread)
        fbPtr:        instance._wasm_framebuffer_ptr(),
        fbWidth:      instance._wasm_framebuffer_width(),
        fbHeight:     instance._wasm_framebuffer_height(),
    });

    // wasm_boot enters MMBasic_RunPromptLoop which never returns under
    // normal operation — ASYNCIFY unwinds at each emscripten_sleep, so
    // this worker's event loop keeps running and onmessage continues
    // to fire between sleeps.
    instance.ccall('wasm_boot', null, [], [], { async: true })
        .catch((e) => self.postMessage({ type: 'log', level: 'warn', line: 'wasm_boot rejected: ' + e }));
}

self.onmessage = (e) => {
    const msg = e.data;
    if (!msg || !msg.type) return;

    if (msg.type === 'init') {
        initInstance(msg.cfg || {});
        return;
    }
    if (!instance) return;  // everything else needs a running instance

    switch (msg.type) {
        case 'key':
            instance._wasm_push_key(msg.code);
            break;

        case 'fs-write':
            try { instance.FS.writeFile(msg.path, new Uint8Array(msg.data)); }
            catch (err) { self.postMessage({ type: 'log', level: 'warn', line: 'fs-write failed: ' + err }); }
            break;

        case 'fs-list':
            try {
                const names = instance.FS.readdir(msg.dir).filter(n => n !== '.' && n !== '..');
                self.postMessage({ type: 'fs-list-result', reqId: msg.reqId, dir: msg.dir, names });
            } catch (err) {
                self.postMessage({ type: 'fs-list-result', reqId: msg.reqId, dir: msg.dir, names: [], error: String(err) });
            }
            break;

        case 'fs-read':
            try {
                const data = instance.FS.readFile(msg.path);
                self.postMessage({ type: 'fs-read-result', reqId: msg.reqId, path: msg.path, data: data.buffer }, [data.buffer]);
            } catch (err) {
                self.postMessage({ type: 'fs-read-result', reqId: msg.reqId, path: msg.path, error: String(err) });
            }
            break;

        case 'fs-unlink':
            try { instance.FS.unlink(msg.path); }
            catch (err) { self.postMessage({ type: 'log', level: 'warn', line: 'fs-unlink failed: ' + err }); }
            break;
    }
};
