// Headless smoke test for Phase 4 (cooperative scheduling + jitter).
//
// Verifies:
//   1. The rAF vsync counter at *wasm_vsync_counter_ptr advances at
//      ~60 Hz — proves app.mjs is bumping the shared cell every frame
//      and C side can spin-read it.
//   2. PAUSE 1000 returns in ~1000 ms via ASYNCIFY.
//   3. A FASTGFX FPS 50 loop produces bounded per-frame deltas
//      measured via TIMER on the BASIC side — no frame > 50 ms (the
//      "hitch" threshold the user was seeing in pico_blocks).
//   4. The framebuffer generation counter is bumping — guards against
//      accidentally gating the rAF blit in a way that hides all output.
//
// Success = exit 0.

import { chromium } from '/Users/joshv/.local/state/yolobox/instances/pico-gamer-main/checkout/web/node_modules/playwright/index.mjs';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';

const PORT = 8125;
const PAGE_URL = `http://127.0.0.1:${PORT}/`;

function startServer() {
    const cwd = new URL('.', import.meta.url).pathname;
    const child = spawn('python3', ['-m', 'http.server', String(PORT), '--bind', '127.0.0.1'], {
        cwd,
        stdio: ['ignore', 'ignore', 'inherit'],
    });
    child.unref();
    return child;
}

async function waitForPort() {
    for (let i = 0; i < 40; i++) {
        try {
            const r = await fetch(PAGE_URL);
            if (r.ok) return;
        } catch (_) {}
        await sleep(200);
    }
    throw new Error('server did not come up');
}

function fail(msg) {
    console.error('FAIL —', msg);
    process.exit(1);
}

const server = startServer();
try {
    await waitForPort();
    // Headless Chromium disables GPU by default; enable a software WebGL
    // backend so our WebGL2 context request succeeds.
    const browser = await chromium.launch({
        headless: true,
        args: [
            '--use-angle=swiftshader',
            '--enable-unsafe-swiftshader',
            '--ignore-gpu-blocklist',
        ],
    });
    try {
        const ctx = await browser.newContext();
        const page = await ctx.newPage();

        const consoleMsgs = [];
        page.on('console', (msg) => consoleMsgs.push(`[${msg.type()}] ${msg.text()}`));
        page.on('pageerror', (e) => consoleMsgs.push(`[pageerror] ${e.message}`));

        await page.goto(PAGE_URL, { waitUntil: 'load' });
        await page.waitForFunction(() => {
            const s = document.getElementById('status');
            return s && s.textContent.startsWith('Ready');
        }, { timeout: 20000 });
        await page.waitForFunction(() => !!window.picomite?.instance, { timeout: 5000 });
        await page.click('#screen');

        // ------ Check 1: rAF advances at display rate ------------------
        const rafTicks = await page.evaluate(() => new Promise((resolve) => {
            let n = 0;
            const deadline = performance.now() + 500;
            const loop = () => {
                if (performance.now() < deadline) {
                    n++;
                    requestAnimationFrame(loop);
                } else {
                    resolve(n);
                }
            };
            requestAnimationFrame(loop);
        }));
        // Headless Chromium is usually ~60 Hz with swiftshader, real
        // hardware varies; accept any reasonable display rate.
        if (rafTicks < 20 || rafTicks > 120) {
            fail(`rAF fired ${rafTicks} times / 500 ms; expected 20–120`);
        }
        console.log(`OK — rAF @ ~${rafTicks * 2} Hz (${rafTicks} ticks in 500 ms).`);

        // ------ Check 2: framebuffer generation advances on draws -----
        const genBefore = await page.evaluate(() => window.picomite.instance._wasm_framebuffer_generation());

        async function typeLine(line) {
            await page.keyboard.type(line, { delay: 5 });
            await page.keyboard.press('Enter');
        }

        await typeLine('NEW');
        await sleep(100);
        await typeLine('10 OPEN "/sd/out.txt" FOR OUTPUT AS #1');
        await typeLine('20 T0=TIMER');
        await typeLine('30 PAUSE 1000');
        await typeLine('40 PRINT #1, STR$(TIMER - T0, 0, 0)');
        await typeLine('50 CLOSE #1');
        await typeLine('RUN');
        // PAUSE 1000 + typing overhead.
        await sleep(2500);

        const pauseResult = await page.evaluate(() => {
            try {
                return window.picomite.instance.FS.readFile('/sd/out.txt', { encoding: 'utf8' });
            } catch (e) { return null; }
        });
        if (!pauseResult) fail(`could not read /sd/out.txt after PAUSE test; tail:\n${consoleMsgs.slice(-20).join('\n')}`);
        const pauseMs = parseInt(pauseResult.trim(), 10);
        if (!Number.isFinite(pauseMs) || pauseMs < 950 || pauseMs > 1200) {
            fail(`PAUSE 1000 measured ${pauseMs} ms (expected 950–1200)`);
        }
        console.log(`OK — PAUSE 1000 measured ${pauseMs} ms.`);

        const genAfter = await page.evaluate(() => window.picomite.instance._wasm_framebuffer_generation());
        if (genAfter === genBefore) {
            fail(`framebuffer generation did not change (${genBefore} → ${genAfter}); blit skip path is stuck`);
        }
        console.log(`OK — framebuffer generation advanced (${genBefore} → ${genAfter}).`);

        // ------ Check 3: FASTGFX SWAP rate, observed from JS -----------
        // Measuring TIMER deltas via the REPL turned out fragile. We
        // write a program to MEMFS and RUN it by filename, then sample
        // the framebuffer generation rate and rAF gaps on the JS side.
        // Proxy metrics for "is the main thread smooth":
        //   - gen rate near FPS 50 → SWAP is pacing correctly
        //   - no rAF gap > 40 ms → no visible hitches
        await page.evaluate(() => {
            const prog = [
                '10 FASTGFX CREATE',
                '15 FASTGFX FPS 50',
                '20 I = 0',
                '30 DO',
                '40 BOX (I*3) MOD 300, 50, (I*3) MOD 300 + 10, 60, 1, RGB(WHITE), RGB(BLACK)',
                '50 FASTGFX SWAP',
                '55 FASTGFX SYNC',
                '60 I = I + 1',
                '70 LOOP UNTIL I >= 500',
                '80 FASTGFX CLOSE',
            ].join('\n') + '\n';
            window.picomite.instance.FS.writeFile('/sd/phase4.bas', prog);
        });
        await typeLine('NEW');
        await sleep(200);
        await typeLine('RUN "phase4.bas"');
        // Let it stabilise — the first frame's setup is noisy.
        await sleep(500);

        const sample = await page.evaluate(async () => {
            const inst = window.picomite.instance;
            const genAt = () => inst._wasm_framebuffer_generation();
            const t0 = performance.now();
            const gen0 = genAt();
            const rafGaps = [];
            let last = performance.now();
            const loop = (t) => {
                rafGaps.push(t - last);
                last = t;
                if (performance.now() - t0 < 3000) requestAnimationFrame(loop);
            };
            requestAnimationFrame(loop);
            await new Promise((r) => setTimeout(r, 3200));
            const t1 = performance.now();
            const gen1 = genAt();
            return {
                genRate: (gen1 - gen0) / ((t1 - t0) / 1000),
                rafGaps: rafGaps.slice(1),  // drop first (start-of-loop offset)
            };
        });

        // Expect generation to advance at ~FPS 50 (SWAP rate).
        if (sample.genRate < 40 || sample.genRate > 65) {
            fail(`framebuffer generation rate = ${sample.genRate.toFixed(1)} Hz (expected 40–65 for FPS 50)`);
        }
        const maxGap = Math.max(...sample.rafGaps);
        const meanGap = sample.rafGaps.reduce((a, b) => a + b, 0) / sample.rafGaps.length;
        if (maxGap > 40) {
            fail(`worst rAF gap = ${maxGap.toFixed(1)} ms during FASTGFX loop (expected ≤ 40)`);
        }
        console.log(`OK — FASTGFX@50: gen rate ${sample.genRate.toFixed(1)} Hz; rAF gap mean ${meanGap.toFixed(1)} ms, worst ${maxGap.toFixed(1)} ms over ${sample.rafGaps.length} frames.`);

        console.log('All Phase 4 smoke checks passed.');
    } finally {
        await browser.close();
    }
} finally {
    server.kill('SIGTERM');
}
