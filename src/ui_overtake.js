/*
 * Oversmack — full-surface overtake UI
 *
 * Smack's engine (same dsp.so as smack-in: mic/line input) with the whole
 * Move surface as a step-FX editor. Loaded from the overtake menu
 * (Shift+Vol+Jog-Click); Back suspends the UI while the DSP keeps
 * processing (suspend_keeps_js); Shift+Vol+Jog-Click exits.
 *
 * Surface map:
 *   Steps 1-16            slice pattern (color per effect, white playhead);
 *                         press = select a slice for editing
 *   Pad rows 2-4 (76-99)  effect palette: pad 76 = Clean, 77-98 = the 22
 *                         effects, pad 99 (top-right) = Unlock (back to
 *                         the seeded roll). Tap to assign to the selected
 *                         slice; re-tap the same effect to lock it as-is.
 *   Bottom row (68-75)    68 Capture / 69 Arm / 70 A-B / 71 Re-Roll /
 *                         72 Clear; hardware Capture button = retro grab
 *   Knobs 1-8             FX Density, Order, Loop Len, Slice Res,
 *                         Wet, Pitch Range, AB Quantize, Seed
 *   Play button           passed through to Move (transport + clock
 *                         keep working under this UI)
 *
 * Talks to the DSP via host_module_set_param/get_param, which the shadow
 * UI shims to the slot-0 overtake DSP ("overtake_dsp:" params).
 * Screen reader: everything announced via shared/screen_reader.mjs.
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

import {
    announce, announceParameter, announceView
} from '/data/UserData/schwung/shared/screen_reader.mjs';


/* Bottom pad row: transport */
const PAD_CAPTURE = 68;
const PAD_ARM     = 69;
const PAD_AB      = 70;
const PAD_REROLL  = 71;
const PAD_CLEAR   = 72;

/* Upper three pad rows: effect palette. Pad 76 + code, codes 0..22;
 * top-right pad 99 = unlock (restore the seeded effect). */
const PAD_PALETTE_FIRST = 76;
const PAD_UNLOCK        = 99;

const STEP_FIRST = 16;
const STEP_COUNT = 16;

/* Color / name / speech per fx code (matches smack_fx_t order in the DSP) */
const FX_COLORS = [
    0x10,        /* CLEAN     — dim white */
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
    YellowGreen, /* FILTER    — sweep family */
    Green,       /* VOWEL */
    OrangeRed,   /* TONALDELAY — buzz/delay family */
    White,       /* FREEZE */
    Cyan,        /* DELAY */
    BrightRed,   /* DIST */
    Purple,      /* PHASER */
    LightGrey    /* VERB */
];

const FX_NAMES = [
    'Clean', 'Retrig', 'Reverse', 'Pitch', 'Speed', 'Gate', 'Buzz',
    'Crush', 'Repeat', 'RevAfter', 'TapeStop', 'TapeStrt', 'Scratch',
    'Envelope', 'Pan', 'Filter', 'Vowel', 'TonalDly', 'Freeze', 'Delay',
    'Distort', 'Phaser', 'Reverb'
];

const FX_SPEECH = [
    'clean', 'retrigger', 'reverse', 'pitch', 'speed', 'gate', 'buzz',
    'bitcrush', 'repeat', 'reverse after', 'tape stop', 'tape start',
    'scratch', 'envelope', 'pan', 'filter', 'vowel', 'tonal delay',
    'freeze', 'delay', 'distortion', 'phaser', 'reverb'
];

const STATE_NAMES = ['IDLE', 'ARMED', 'REC', 'LOOP'];
const STATE_SPEECH = ['idle', 'armed', 'recording', 'looping'];

/* Knobs 1-8 (CC 71-78). Terse screen strings + speech-friendly variants. */
const KNOBS = [
    { key: 'fx_density',    name: 'FX',   min: 0, max: 100, step: 5,
      speech: 'FX Density', unit: ' percent' },
    { key: 'order_density', name: 'Ord',  min: 0, max: 100, step: 5,
      speech: 'Order Density', unit: ' percent' },
    { key: 'loop_len',      name: 'Len',  opts: ['1st', '2st', '1bt', '2bt', '1br', '2br', '4br', '8br', '16b'],
      speech: 'Loop Length',
      speechOpts: ['1 step', '2 steps', '1 beat', '2 beats', '1 bar', '2 bars', '4 bars', '8 bars', '16 bars'] },
    { key: 'slice_res',     name: 'Res',  opts: ['½st', '1st', '2st', '4st'],
      speech: 'Slice Resolution',
      speechOpts: ['half step', '1 step', '2 steps', '4 steps'] },
    { key: 'wet',           name: 'Wet',  min: 0, max: 100, step: 5,
      speech: 'Wet', unit: ' percent' },
    { key: 'pitch_range',   name: 'Pit',  min: 1, max: 12,  step: 1,
      speech: 'Pitch Range', unit: ' semitones' },
    { key: 'quantize',      name: 'Qnt',  opts: ['Inst', 'Slic', 'Loop'],
      speech: 'A B Quantize',
      speechOpts: ['instant', 'slice', 'loop'] },
    { key: 'seed',          name: 'Seed', min: 1, max: 9999, step: 1,
      speech: 'Seed' }
];

let knobValues = [50, 0, 4, 1, 100, 12, 1, 4303];
let state = 0;          /* 0 idle, 1 armed, 2 rec, 3 looping */
let ab = 1;
let pattern = '';
let nSlices = 0;
let playSlice = -1;
let selectedSlice = -1;
let tickCount = 0;
let needsRedraw = true;
let dspReady = false;

function gp(key) {
    const v = host_module_get_param(key);
    return (v === null || v === undefined) ? null : String(v);
}

function fetchAll() {
    const rs = gp('run_state');
    if (rs === null) return false;       /* DSP not up yet — retry in tick */
    for (let i = 0; i < KNOBS.length; i++) {
        const v = gp(KNOBS[i].key);
        if (v !== null) knobValues[i] = parseFloat(v) || 0;
    }
    state = parseInt(rs);
    ab = parseInt(gp('ab') || '1');
    pattern = gp('pattern') || '';
    nSlices = parseInt(gp('n_slices') || '0');
    return true;
}

function sliceFx(i) {
    if (i < 0 || i >= pattern.length) return 0;
    const code = pattern.charCodeAt(i) - 48;
    return (code >= 0 && code < FX_COLORS.length) ? code : 0;
}

function knobDisplay(i) {
    const k = KNOBS[i];
    if (k.opts) {
        const idx = Math.max(0, Math.min(k.opts.length - 1, Math.round(knobValues[i])));
        return k.opts[idx];
    }
    return `${Math.round(knobValues[i])}`;
}

function knobSpeech(i) {
    const k = KNOBS[i];
    if (k.speechOpts) {
        const idx = Math.max(0, Math.min(k.speechOpts.length - 1, Math.round(knobValues[i])));
        return k.speechOpts[idx];
    }
    return `${Math.round(knobValues[i])}${k.unit || ''}`;
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
    announceParameter(k.speech, knobSpeech(i));
    needsRedraw = true;
}

/* ---- LEDs ---- */

function paintTransport(force) {
    setLED(PAD_CAPTURE, Green, force);
    setLED(PAD_ARM, state === 1 || state === 2 ? BrightRed : Red, force);
    setLED(PAD_AB, state === 3 ? (ab ? White : LightGrey) : Black, force);
    setLED(PAD_REROLL, Blue, force);
    setLED(PAD_CLEAR, state === 3 ? OrangeRed : Black, force);
    for (let p = 73; p <= 75; p++) setLED(p, Black, force);
}

function paintPalette(force) {
    for (let c = 0; c < FX_COLORS.length; c++)
        setLED(PAD_PALETTE_FIRST + c, FX_COLORS[c], force);
    setLED(PAD_UNLOCK, 0x2D /* dim blue: unlock */, force);
}

function paintSteps(force) {
    const shown = Math.min(nSlices, STEP_COUNT);
    for (let i = 0; i < STEP_COUNT; i++) {
        let color = Black;
        if (state === 3 && i < shown) {
            color = FX_COLORS[sliceFx(i)];
            /* B side shows the pattern; A side shows all-clean */
            if (!ab) color = FX_COLORS[0];
            if (i === selectedSlice) color = BrightGreen;  /* editing target */
            if (i === playSlice) color = White;            /* playhead wins */
        }
        setLED(STEP_FIRST + i, color, force);
    }
}

function paintAll(force) {
    paintTransport(force);
    paintPalette(force);
    paintSteps(force);
}

/* ---- Screen ---- */

function drawUI() {
    clear_screen();
    let title = 'OVERSMACK  ' + STATE_NAMES[state];
    if (state === 3) title += ab ? ' · B' : ' · A';
    drawHeader(title);

    /* selected-slice line */
    if (state === 3 && selectedSlice >= 0 && selectedSlice < nSlices) {
        print(2, 13, `Step ${selectedSlice + 1}: ${FX_NAMES[sliceFx(selectedSlice)]}`, 1);
    } else if (state === 3) {
        print(2, 13, 'Press a step to edit', 1);
    } else {
        print(2, 13, 'Capture or Arm a loop', 1);
    }

    /* two rows of four knob params */
    for (let i = 0; i < 8; i++) {
        const col = i % 4, row = (i / 4) | 0;
        const x = 2 + col * 32;
        const y = 24 + row * 17;
        print(x, y, KNOBS[i].name, 1);
        print(x, y + 8, knobDisplay(i), 1);
    }

    drawFooter({ left: 'Step:sel Pad:fx', right: 'Back:hide' });
    needsRedraw = false;
}

/* ---- Actions ---- */

function assignFx(code) {
    if (state !== 3 || selectedSlice < 0 || selectedSlice >= nSlices) {
        announce('Select a step first');
        return;
    }
    host_module_set_param(`lock_slice_${selectedSlice}`, `${code}`);
    announce(`Slice ${selectedSlice + 1}, ${FX_SPEECH[code]}`);
    refreshSoon();
}

function unlockSlice() {
    if (state !== 3 || selectedSlice < 0 || selectedSlice >= nSlices) {
        announce('Select a step first');
        return;
    }
    host_module_set_param(`lock_slice_${selectedSlice}`, '-1');
    announce(`Slice ${selectedSlice + 1} unlocked`);
    refreshSoon();
}

/* ---- Lifecycle ---- */

globalThis.init = function() {
    dspReady = fetchAll();
    playSlice = -1;
    selectedSlice = -1;
    paintAll(true);
    needsRedraw = true;
    if (dspReady) {
        let spoken = 'Oversmack, ' + STATE_SPEECH[state];
        if (state === 3) spoken += ab ? ', side B' : ', side A';
        announceView(spoken);
    }
};

globalThis.onResume = function() {
    /* LEDs were cleared while suspended: full forced repaint */
    dspReady = fetchAll();
    paintAll(true);
    needsRedraw = true;
    announceView('Oversmack');
};

globalThis.tick = function() {
    tickCount++;

    if (!dspReady) {
        dspReady = fetchAll();
        if (dspReady) {
            paintAll(true);
            needsRedraw = true;
            announceView('Oversmack, ' + STATE_SPEECH[state]);
        } else if (needsRedraw) {
            drawUI();
        }
        return;
    }

    /* playhead chase: cheap single get_param per tick */
    if (state === 3) {
        const ps = parseInt(gp('play_slice') || '-1');
        if (ps !== playSlice) {
            playSlice = ps;
            paintSteps(false);
        }
    }

    /* periodic state/pattern refresh (knob edits, quantized AB flips) */
    if (tickCount % 12 === 0) {
        const oldState = state, oldAb = ab, oldPattern = pattern;
        state = parseInt(gp('run_state') || '0');
        ab = parseInt(gp('ab') || '1');
        pattern = gp('pattern') || '';
        nSlices = parseInt(gp('n_slices') || '0');
        if (state !== 3) selectedSlice = -1;
        /* speak async transitions (armed -> recording -> looping); A/B is
         * announced at press time (applied value lags a quantized flip) */
        if (state !== oldState)
            announce(state === 3 ? `looping, ${nSlices} slices` : STATE_SPEECH[state]);
        if (state !== oldState || ab !== oldAb || pattern !== oldPattern) {
            paintTransport(false);
            paintSteps(false);
            needsRedraw = true;
        }
    }

    if (needsRedraw) drawUI();
};

globalThis.onMidiMessageInternal = function(data) {
    const status = data[0] & 0xF0;
    const d1 = data[1];
    const d2 = data[2];

    if (status === 0xB0) {
        /* Capture button = retro grab */
        if (d1 === MoveCapture && d2 >= 64) {
            host_module_set_param('capture', '1');
            announce('Capture');
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
        if (d1 === PAD_CAPTURE) { host_module_set_param('capture', '1'); announce('Capture'); refreshSoon(); return; }
        if (d1 === PAD_ARM)     { host_module_set_param('arm', '1');     announce('Arm');     refreshSoon(); return; }
        if (d1 === PAD_REROLL)  { host_module_set_param('reroll', '1');  announce('Re-roll'); refreshSoon(); return; }
        if (d1 === PAD_CLEAR)   { host_module_set_param('clear', '1');   announce('Clear');   refreshSoon(); return; }
        if (d1 === PAD_AB) {
            ab = ab ? 0 : 1;
            host_module_set_param('ab', `${ab}`);
            announce(ab ? 'B, pattern' : 'A, clean loop');
            paintTransport(false);
            paintSteps(false);
            refreshSoon();
            return;
        }

        /* Effect palette */
        if (d1 === PAD_UNLOCK) { unlockSlice(); return; }
        if (d1 >= PAD_PALETTE_FIRST && d1 < PAD_PALETTE_FIRST + FX_COLORS.length) {
            assignFx(d1 - PAD_PALETTE_FIRST);
            return;
        }

        /* Step press: select a slice for editing */
        if (d1 >= STEP_FIRST && d1 < STEP_FIRST + STEP_COUNT) {
            const i = d1 - STEP_FIRST;
            if (state === 3 && i < nSlices) {
                selectedSlice = i;
                announce(`Slice ${i + 1}, ${FX_SPEECH[sliceFx(i)]}`);
                paintSteps(false);
                needsRedraw = true;
            } else if (state !== 3) {
                announce('No loop yet');
            }
            return;
        }
    }
};

globalThis.onUnload = function() {
    /* Host unloads the slot-0 DSP and restores LEDs; nothing to release. */
};

/* pull fresh state on the next periodic refresh instead of immediately —
 * quantized actions (AB at loop boundary, armed record) land later anyway */
function refreshSoon() {
    tickCount = -1; /* forces the %12 refresh on next tick */
}
