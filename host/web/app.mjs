// Phase 0 placeholder loader. Imports the emscripten module, captures
// stdout/stderr into the #out pane, and awaits module bring-up. Phase 1
// swaps this for an xterm.js wrapper + wasm_push_key / wasm_boot wiring.

import Module from './picomite.mjs';

const out = document.getElementById('out');
let buffer = '';

function append(prefix, line) {
    buffer += prefix + line + '\n';
    out.textContent = buffer;
}

try {
    await Module({
        print: (line) => append('', line),
        printErr: (line) => append('[err] ', line),
    });
} catch (err) {
    append('[fatal] ', String(err));
    throw err;
}
