// WebAudio engine for the PicoCalc simulator.
//
// Receives JSON commands over the same /ws WebSocket that drives the
// canvas. Messages this module handles:
//
//   { op: "tone",   l: <Hz>, r: <Hz>, ms?: <duration> }
//   { op: "stop"   }
//   { op: "sound",  slot: 1..4, ch: "L"|"R"|"B", type: "S|Q|T|W|O|P|N",
//                   f: <Hz>, vol: 0..25 }
//   { op: "volume", l: 0..100, r: 0..100 }
//   { op: "pause"  }
//   { op: "resume" }
//
// PLAY TONE and PLAY SOUND are intentionally independent in PicoMite —
// one is a fire-and-forget pair of sine oscillators, the other is four
// slot-oscillators per stereo channel with waveform selection. The code
// keeps them apart so a TONE in the middle of a SOUND doesn't cross-
// contaminate.

const WAVE_FOR_TYPE = {
    "S": "sine",
    "Q": "square",
    "T": "triangle",
    "W": "sawtooth",
};

let ctx = null;
let masterL = null, masterR = null;
let merger = null;

// PLAY TONE state
let toneL = null, toneR = null;
let toneGainL = null, toneGainR = null;
let toneStopTimer = null;

// PLAY SOUND state — 4 slots × {L, R} oscillators + per-channel gains.
// Noise slots use an AudioBufferSourceNode instead of OscillatorNode.
const soundSlots = [null, null, null, null];

// Stay suspended until we see a user gesture; Safari/Firefox require it.
let gestureResumed = false;

function ensureCtx() {
    if (ctx) return ctx;
    const C = window.AudioContext || window.webkitAudioContext;
    if (!C) { console.warn("WebAudio not supported"); return null; }
    ctx = new C();

    // Master L/R gains fed into a stereo merger. Default master = 1.0
    // (= PLAY VOLUME 100). PicoMite's "logarithmic" 0..100 is mapped
    // as gain = (v/100)^2 so 50 feels about half as loud as 100.
    masterL = ctx.createGain();
    masterR = ctx.createGain();
    masterL.gain.value = 1.0;
    masterR.gain.value = 1.0;
    merger = ctx.createChannelMerger(2);
    masterL.connect(merger, 0, 0);
    masterR.connect(merger, 0, 1);
    merger.connect(ctx.destination);
    return ctx;
}

// Browsers block AudioContext.state from becoming "running" unless the
// context was created or resumed during a user gesture. Creating the
// context lazily inside a WebSocket message handler does NOT count as a
// gesture — the context stays suspended and nothing plays until the
// next keydown happens to resume it. So we create the context eagerly
// on the very first user gesture and resume on every subsequent one
// (covers tab-switch back into the page).
function armAudioOnGesture() {
    if (gestureResumed) return;
    gestureResumed = true;
    const prime = () => {
        ensureCtx();
        if (ctx && ctx.state === "suspended") ctx.resume();
    };
    // Capture-phase, in case something downstream stops propagation.
    window.addEventListener("keydown",    prime, { capture: true });
    window.addEventListener("mousedown",  prime, { capture: true });
    window.addEventListener("touchstart", prime, { capture: true });
    window.addEventListener("pointerdown", prime, { capture: true });
}

// ---- PLAY TONE -----------------------------------------------------------

function stopTone() {
    if (toneStopTimer) { clearTimeout(toneStopTimer); toneStopTimer = null; }
    for (const n of [toneL, toneR]) { if (n) try { n.stop(); n.disconnect(); } catch (e) {} }
    for (const g of [toneGainL, toneGainR]) { if (g) try { g.disconnect(); } catch (e) {} }
    toneL = toneR = toneGainL = toneGainR = null;
}

function playTone(leftHz, rightHz, ms) {
    if (!ensureCtx()) return;
    stopTone();

    // A zero frequency is the device's "no tone this side" case; don't
    // create an oscillator that side.
    if (leftHz > 0) {
        toneL = ctx.createOscillator();
        toneL.type = "sine";
        toneL.frequency.value = leftHz;
        toneGainL = ctx.createGain();
        toneGainL.gain.value = 0.35;
        toneL.connect(toneGainL).connect(masterL);
        toneL.start();
    }
    if (rightHz > 0) {
        toneR = ctx.createOscillator();
        toneR.type = "sine";
        toneR.frequency.value = rightHz;
        toneGainR = ctx.createGain();
        toneGainR.gain.value = 0.35;
        toneR.connect(toneGainR).connect(masterR);
        toneR.start();
    }

    if (typeof ms === "number" && ms > 0) {
        toneStopTimer = setTimeout(stopTone, ms);
    }
}

// ---- PLAY SOUND ----------------------------------------------------------

// Build one noise buffer, reused by every noise slot. "P" (periodic)
// uses a short looping buffer so the ear hears a rough pitch; "N" uses
// one long uncorrelated buffer for pure white noise.
let whiteNoiseBuf = null;
function getWhiteNoiseBuffer() {
    if (whiteNoiseBuf) return whiteNoiseBuf;
    const sec = 1.0;
    whiteNoiseBuf = ctx.createBuffer(1, sec * ctx.sampleRate, ctx.sampleRate);
    const data = whiteNoiseBuf.getChannelData(0);
    for (let i = 0; i < data.length; ++i) data[i] = Math.random() * 2 - 1;
    return whiteNoiseBuf;
}

function makePeriodicNoiseBuffer(freq) {
    // Length = period rounded to sample count, at least 8 samples.
    const samples = Math.max(8, Math.floor(ctx.sampleRate / Math.max(1, freq)));
    const buf = ctx.createBuffer(1, samples, ctx.sampleRate);
    const data = buf.getChannelData(0);
    for (let i = 0; i < samples; ++i) data[i] = Math.random() * 2 - 1;
    return buf;
}

function stopSlotSide(slot, side /* "L"|"R" */) {
    if (!slot) return;
    const n = slot[side].node;
    const g = slot[side].gain;
    if (n) try { n.stop(); n.disconnect(); } catch (e) {}
    if (g) try { g.disconnect(); } catch (e) {}
    slot[side].node = null;
    slot[side].gain = null;
}

function stopAllSounds() {
    for (let i = 0; i < soundSlots.length; ++i) {
        const s = soundSlots[i];
        if (!s) continue;
        stopSlotSide(s, "L");
        stopSlotSide(s, "R");
        soundSlots[i] = null;
    }
}

function setSoundSide(slotIdx, side /* "L"|"R" */, type, freq, vol) {
    if (!soundSlots[slotIdx]) {
        soundSlots[slotIdx] = {
            L: { node: null, gain: null },
            R: { node: null, gain: null }
        };
    }
    const slot = soundSlots[slotIdx];
    stopSlotSide(slot, side);

    // type "O" means silence this side — we stopped, done.
    if (type === "O") return;

    // 4 voices × 25 max = cap; /4 so 4 full-volume slots don't clip.
    const gainVal = (vol / 25) * 0.25;
    const gain = ctx.createGain();
    gain.gain.value = gainVal;

    let src;
    if (type === "N") {
        src = ctx.createBufferSource();
        src.buffer = getWhiteNoiseBuffer();
        src.loop = true;
    } else if (type === "P") {
        src = ctx.createBufferSource();
        src.buffer = makePeriodicNoiseBuffer(freq);
        src.loop = true;
    } else if (type === "U") {
        // User-defined waveform — not implemented; fall back to sine.
        src = ctx.createOscillator();
        src.type = "sine";
        src.frequency.value = freq;
    } else {
        src = ctx.createOscillator();
        src.type = WAVE_FOR_TYPE[type] || "sine";
        src.frequency.value = freq;
    }

    const master = (side === "L") ? masterL : masterR;
    src.connect(gain).connect(master);
    src.start();
    slot[side].node = src;
    slot[side].gain = gain;
}

function playSound(slot1based, ch, type, freq, vol) {
    if (!ensureCtx()) return;
    const idx = slot1based - 1;
    if (idx < 0 || idx >= 4) return;
    if (ch === "L" || ch === "B") setSoundSide(idx, "L", type, freq, vol);
    if (ch === "R" || ch === "B") setSoundSide(idx, "R", type, freq, vol);
}

// ---- PLAY VOLUME / PAUSE / RESUME / STOP ---------------------------------

function playStop() {
    stopTone();
    stopAllSounds();
}

function volToGain(v) {
    const x = Math.max(0, Math.min(100, v)) / 100;
    return x * x;
}

function setVolume(l, r) {
    if (!ensureCtx()) return;
    masterL.gain.value = volToGain(l);
    masterR.gain.value = volToGain(r);
}

function pauseAudio() { if (ctx) ctx.suspend(); }
function resumeAudio() { if (ctx) ctx.resume(); }

// ---- Dispatch ------------------------------------------------------------

export function handleAudioMessage(msg) {
    switch (msg.op) {
        case "tone":
            playTone(+msg.l || 0, +msg.r || 0,
                     (typeof msg.ms === "number") ? msg.ms : undefined);
            break;
        case "stop":   playStop(); break;
        case "sound":  playSound(+msg.slot, msg.ch, msg.type, +msg.f, +msg.vol); break;
        case "volume": setVolume(+msg.l, +msg.r); break;
        case "pause":  pauseAudio(); break;
        case "resume": resumeAudio(); break;
    }
}

armAudioOnGesture();
