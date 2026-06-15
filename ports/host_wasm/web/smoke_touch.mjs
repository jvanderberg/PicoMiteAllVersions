// Headless WASM smoke for mouse-emulated GUI-control touch in the browser.
//
// Boots the page, enables GUI controls, RUNs a program that creates a GUI
// SWITCH and writes its CtrlVal to /sd/sw.txt whenever it changes, then
// dispatches a real mouse click on the canvas at the switch location. A
// pass requires the switch value to flip — proving the whole path works:
//   pointer event -> coord map -> shared touch block -> 1ms tick edge ->
//   ProcessTouch -> switch toggle -> CtrlVal.

import { chromium } from '/Users/joshv/.local/state/yolobox/instances/pico-gamer-main/checkout/web/node_modules/playwright/index.mjs';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';

const PORT = 8137;
const PAGE_URL = `http://127.0.0.1:${PORT}/`;
const ENTER = 0x0D;

function startServer() {
    const cwd = new URL('.', import.meta.url).pathname;
    const child = spawn('python3', ['./serve.py', String(PORT)], {
        cwd, stdio: ['ignore', 'ignore', 'inherit'],
    });
    child.unref();
    return child;
}
async function waitForPort() {
    for (let i = 0; i < 40; i++) {
        try { const r = await fetch(PAGE_URL); if (r.ok) return; } catch {}
        await sleep(200);
    }
    throw new Error('server did not come up');
}
function fail(msg) { console.error('FAIL —', msg); process.exit(1); }

async function pushKeyCodes(page, codes) {
    await page.evaluate((arr) => {
        for (const c of arr) window.picomite.worker.postMessage({ type: 'key', code: c });
    }, codes);
}
async function runImmediate(page, command, settleMs = 400) {
    await pushKeyCodes(page, [...command].map(c => c.charCodeAt(0)).concat([ENTER]));
    await sleep(settleMs);
}
async function fsRead(page, path) {
    return await page.evaluate(async (p) => {
        try { return Array.from(await window.picomite.fsRead(p)); } catch { return null; }
    }, path);
}

// Switch geometry in framebuffer pixels: x 120..300, y 120..190.
const SW_FB_CX = 210, SW_FB_CY = 155;

const PROGRAM = [
    'GUI SWITCH #1, "SW", 120, 120, 180, 70',
    'last = CtrlVal(#1)',
    'OPEN "sw.txt" FOR OUTPUT AS #2 : PRINT #2, "INIT " + STR$(last) : CLOSE #2',
    'DO',
    '  v = CtrlVal(#1)',
    '  IF v <> last THEN',
    '    last = v',
    '    OPEN "sw.txt" FOR OUTPUT AS #2 : PRINT #2, "CHG " + STR$(v) : CLOSE #2',
    '  ENDIF',
    'LOOP',
].join('\n') + '\n';

const server = startServer();
let exitCode = 0;
try {
    await waitForPort();
    const browser = await chromium.launch({
        headless: true,
        args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'],
    });
    try {
        const ctx = await browser.newContext();
        const page = await ctx.newPage();
        const errors = [];
        page.on('pageerror', (e) => errors.push(`[pageerror] ${e.message}`));

        await page.goto(PAGE_URL, { waitUntil: 'load' });
        if (!await page.evaluate(() => window.crossOriginIsolated)) fail('crossOriginIsolated = false');
        await page.waitForFunction(() => !!window.picomite?.worker, { timeout: 25000 });
        const hasTouch = await page.evaluate(() => !!window.picomite); // touchBaseIdx wired internally
        await sleep(1500); // banner

        await page.click('#screen');

        // Enable GUI controls (immediate mode; SoftReset is a no-op on host).
        await runImmediate(page, 'OPTION GUI CONTROLS 20', 500);

        // Drop any stale result, write + run the program.
        await page.evaluate(() => window.picomite.worker.postMessage({ type: 'fs-unlink', path: '/sd/sw.txt' }));
        await page.evaluate((src) => window.picomite.fsWrite('/sd/g.bas', new TextEncoder().encode(src)), PROGRAM);
        await sleep(200);
        await runImmediate(page, 'RUN "g.bas"', 800);

        // Confirm the program is running and produced the INIT marker.
        let init = null;
        for (let i = 0; i < 20; i++) {
            const b = await fsRead(page, '/sd/sw.txt');
            if (b) { init = String.fromCharCode(...b); break; }
            await sleep(150);
        }
        if (!init) fail(`switch program never wrote INIT (page errors: ${errors.join(' | ')})`);
        console.log('  init:', JSON.stringify(init.trim()));

        // Dispatch a real mouse click on the switch. Hold it long enough for
        // the 50 ms ProcessTouch settle to elapse before release.
        const box = await page.evaluate(() => {
            const r = document.getElementById('screen').getBoundingClientRect();
            return { left: r.left, top: r.top, width: r.width, height: r.height };
        });
        const cx = box.left + (SW_FB_CX / 320) * box.width;
        const cy = box.top  + (SW_FB_CY / 320) * box.height;
        await page.mouse.move(cx, cy);
        await page.mouse.down();
        await sleep(300);
        await page.mouse.up();
        await sleep(500);

        let after = null;
        for (let i = 0; i < 25; i++) {
            const b = await fsRead(page, '/sd/sw.txt');
            if (b) { const s = String.fromCharCode(...b).trim(); if (s.startsWith('CHG')) { after = s; break; } }
            await sleep(150);
        }

        if (after) {
            console.log('  after click:', JSON.stringify(after));
            console.log('\nPASS — switch toggled via mouse-as-touch');
        } else {
            console.log('\nFAIL — switch did not toggle after click');
            exitCode = 1;
        }
        if (errors.length) { console.log('\nPage errors:'); for (const e of errors) console.log('  ' + e); }
    } finally {
        await browser.close();
    }
} finally {
    server.kill('SIGTERM');
}
process.exit(exitCode);
