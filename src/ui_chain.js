/*
 * Smack — Signal Chain UI
 *
 * Loads when Smack's component editor is opened inside a slot's Signal
 * Chain (the Master FX editor uses auto-generated knob pages instead).
 *
 * Controls while this screen is open:
 *   Pad 68 (bottom-left)  CAPTURE — retro-grab the last loop-length   [green]
 *   Pad 69                ARM     — record from next step boundary    [red]
 *   Pad 70                A/B     — clean loop vs pattern             [white/grey]
 *   Pad 71                RE-ROLL — new random pattern                [blue]
 *   Pad 76 (above 68)     CLEAR   — drop the loop                     [dim orange]
 *   Capture button (CC52) same as pad 68
 *   Steps 1-16            pattern display (color = effect per slice);
 *                         press to mute a slice's effect, press again
 *                         to restore the seeded one
 *   Knobs 1-8             FX Density, Order, Loop Len, Slice Res,
 *                         Wet, Pitch Range, AB Quantize, Seed
 */

import {
    MoveKnob1, MoveCapture,
    Black, White, LightGrey, Red, BrightRed, Blue, Green, BrightGreen,
    Cyan, Purple, YellowGreen, OrangeRed
} from '/data/UserData/schwung/shared/constants.mjs';

import { decodeDelta, setLED } from '/data/UserData/schwung/shared/input_filter.mjs';

import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter
} from '/data/UserData/schwung/shared/menu_layout.mjs';


/* Transport pads (bottom-left corner of the 8x4 grid) */
const PAD_CAPTURE = 68;
const PAD_ARM     = 69;
const PAD_AB      = 70;
const PAD_REROLL  = 71;
const PAD_CLEAR   = 76;

/* Step buttons show the first 16 slices */
const STEP_FIRST = 16;
const STEP_COUNT = 16;

/* Slice LED color by effect code (matches smack_fx_t order in the DSP) */
const FX_COLORS = [
    0x10,        /* NONE      — dim white: slice exists, plays clean */
    Red,         /* RETRIG */
    Blue,        /* REVERSE */
    Purple,      /* PITCH */
    Cyan,        /* SPEED */
    YellowGreen, /* GATE */
    OrangeRed,   /* BUZZ */
    BrightGreen, /* CRUSH */
    BrightRed,   /* REPEAT */
    Blue,        /* REVAFTER  — reverse family */
    LightGrey,   /* TAPESTOP */
    Green,       /* TAPESTART */
    Purple,      /* SCRATCH   — pitch family */
    0x30,        /* ENV       — mid white */
    Cyan,        /* PAN       — stereo/speed family */
    YellowGreen  /* FILTER    — sweep family */
];

const STATE_NAMES = ['IDLE', 'ARMED', 'REC', 'LOOP'];

/* Knobs 1-8 (CC 71-78) left to right */
const KNOBS = [
    { key: 'fx_density',    name: 'FX',   min: 0, max: 100, step: 5 },
    { key: 'order_density', name: 'Ord',  min: 0, max: 100, step: 5 },
    { key: 'loop_len',      name: 'Len',  opts: ['1st', '2st', '1bt', '2bt', '1br', '2br', '4br', '8br', '16b'] },
    { key: 'slice_res',     name: 'Res',  opts: ['½st', '1st', '2st', '4st'] },
    { key: 'wet',           name: 'Wet',  min: 0, max: 100, step: 5 },
    { key: 'pitch_range',   name: 'Pit',  min: 1, max: 12,  step: 1 },
    { key: 'quantize',      name: 'Qnt',  opts: ['Inst', 'Slic', 'Loop'] },
    { key: 'seed',          name: 'Seed', min: 1, max: 9999, step: 1 }
];

let knobValues = [50, 0, 4, 1, 100, 12, 1, 24301];
let state = 0;          /* 0 idle, 1 armed, 2 rec, 3 looping */
let ab = 1;
let pattern = '';
let nSlices = 0;
let playSlice = -1;
let tickCount = 0;
let needsRedraw = true;

function gp(key) {
    const v = host_module_get_param(key);
    return (v === null || v === undefined) ? null : String(v);
}

function fetchAll() {
    for (let i = 0; i < KNOBS.length; i++) {
        const v = gp(KNOBS[i].key);
        if (v !== null) knobValues[i] = parseFloat(v) || 0;
    }
    state = parseInt(gp('run_state') || '0');
    ab = parseInt(gp('ab') || '1');
    pattern = gp('pattern') || '';
    nSlices = parseInt(gp('n_slices') || '0');
}

function knobDisplay(i) {
    const k = KNOBS[i];
    if (k.opts) {
        const idx = Math.max(0, Math.min(k.opts.length - 1, Math.round(knobValues[i])));
        return k.opts[idx];
    }
    return `${Math.round(knobValues[i])}`;
}

function adjustKnob(i, delta) {
    const k = KNOBS[i];
    const max = k.opts ? k.opts.length - 1 : k.max;
    const min = k.opts ? 0 : k.min;
    const step = k.opts ? 1 : k.step;
    let v = Math.max(min, Math.min(max, knobValues[i] + delta * step));
    if (v === knobValues[i]) return;
    knobValues[i] = v;
    host_module_set_param(k.key, `${Math.round(v)}`);
    needsRedraw = true;
}

/* ---- LEDs ---- */

function updateTransportLEDs() {
    setLED(PAD_CAPTURE, Green);
    setLED(PAD_ARM, state === 1 || state === 2 ? BrightRed : Red);
    setLED(PAD_AB, state === 3 ? (ab ? White : LightGrey) : Black);
    setLED(PAD_REROLL, Blue);
    setLED(PAD_CLEAR, state === 3 ? OrangeRed : Black);
}

function updateStepLEDs() {
    const shown = Math.min(nSlices, STEP_COUNT);
    for (let i = 0; i < STEP_COUNT; i++) {
        let color = Black;
        if (state === 3 && i < shown) {
            const code = pattern.charCodeAt(i) - 48;
            color = FX_COLORS[(code >= 0 && code < FX_COLORS.length) ? code : 0];
            /* B side shows the pattern; A side shows all-clean */
            if (!ab) color = FX_COLORS[0];
            if (i === playSlice) color = White;   /* playhead chase */
        }
        setLED(STEP_FIRST + i, color);
    }
}

/* ---- Screen ---- */

function drawUI() {
    clear_screen();
    let title = 'SMACK  ' + STATE_NAMES[state];
    if (state === 3) title += ab ? ' · B' : ' · A';
    drawHeader(title);

    /* two rows of four knob params */
    for (let i = 0; i < 8; i++) {
        const col = i % 4, row = (i / 4) | 0;
        const x = 2 + col * 32;
        const y = 15 + row * 20;
        print(x, y, KNOBS[i].name, 1);
        print(x, y + 8, knobDisplay(i), 1);
    }

    /* pattern summary line */
    if (state === 3) {
        let fxCount = 0;
        for (let i = 0; i < pattern.length; i++)
            if (pattern[i] !== '0') fxCount++;
        print(2, 55, `${nSlices} slices  ${fxCount} fx`, 1);
    }

    drawFooter({ left: 'Cap Arm A/B Roll', right: 'Step:mute' });
    needsRedraw = false;
}

/* ---- Lifecycle ---- */

function init() {
    fetchAll();
    playSlice = -1;
    updateTransportLEDs();
    updateStepLEDs();
    needsRedraw = true;
}

function tick() {
    tickCount++;

    /* playhead chase: cheap single get_param per tick */
    if (state === 3) {
        const ps = parseInt(gp('play_slice') || '-1');
        if (ps !== playSlice) {
            playSlice = ps;
            updateStepLEDs();
        }
    }

    /* periodic state/pattern refresh (knob edits, quantized AB flips) */
    if (tickCount % 12 === 0) {
        const oldState = state, oldAb = ab, oldPattern = pattern;
        state = parseInt(gp('run_state') || '0');
        ab = parseInt(gp('ab') || '1');
        pattern = gp('pattern') || '';
        nSlices = parseInt(gp('n_slices') || '0');
        if (state !== oldState || ab !== oldAb || pattern !== oldPattern) {
            updateTransportLEDs();
            updateStepLEDs();
            needsRedraw = true;
        }
    }

    if (needsRedraw) drawUI();
}

function onMidiMessageInternal(data) {
    const status = data[0] & 0xF0;
    const d1 = data[1];
    const d2 = data[2];

    if (status === 0xB0) {
        /* Capture button mirrors the capture pad */
        if (d1 === MoveCapture && d2 >= 64) {
            host_module_set_param('capture', '1');
            refreshSoon();
            return;
        }
        /* Knobs 1-8 */
        if (d1 >= MoveKnob1 && d1 < MoveKnob1 + 8) {
            const delta = decodeDelta(d2);
            if (delta !== 0) adjustKnob(d1 - MoveKnob1, delta);
            return;
        }
        return;
    }

    if (status === 0x90 && d2 > 0) {
        /* Transport pads */
        if (d1 === PAD_CAPTURE) { host_module_set_param('capture', '1'); refreshSoon(); return; }
        if (d1 === PAD_ARM)     { host_module_set_param('arm', '1');     refreshSoon(); return; }
        if (d1 === PAD_REROLL)  { host_module_set_param('reroll', '1');  refreshSoon(); return; }
        if (d1 === PAD_CLEAR)   { host_module_set_param('clear', '1');   refreshSoon(); return; }
        if (d1 === PAD_AB) {
            ab = ab ? 0 : 1;
            host_module_set_param('ab', `${ab}`);
            refreshSoon();
            return;
        }

        /* Step press: mute a slice's effect / restore the seeded one */
        if (d1 >= STEP_FIRST && d1 < STEP_FIRST + STEP_COUNT) {
            const i = d1 - STEP_FIRST;
            if (state === 3 && i < nSlices) {
                const code = pattern.charCodeAt(i) - 48;
                if (code > 0) host_module_set_param(`lock_slice_${i}`, '0');
                else          host_module_set_param(`lock_slice_${i}`, '-1');
                refreshSoon();
            }
            return;
        }
    }
}

/* pull fresh state on the next periodic refresh instead of immediately —
 * quantized actions (AB at loop boundary, armed record) land later anyway */
function refreshSoon() {
    tickCount = -1; /* forces the %12 refresh on next tick */
}

globalThis.chain_ui = {
    init,
    tick,
    onMidiMessageInternal
};
