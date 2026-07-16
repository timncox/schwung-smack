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
 *   Pad rows 2-4 (76-99)  effect palette (layout + per-pad variants come
 *                         from the DSP's `palette` param, arranged in the
 *                         web editor), pad 99 (top-right) = Unlock (back to
 *                         the seeded roll). Tap to assign to the selected
 *                         slice; re-tap the same effect to lock it as-is.
 *   Bottom row (68-75)    68 Capture / 69 Arm / 70 A-B / 71 Re-Roll /
 *                         72 Clear / 73 Monitor (input monitoring on/off,
 *                         doubles as the feedback-guard override) /
 *                         74 Stereo-DualMono toggle / 75 lane select
 *                         (dual mono: tap = edit L or R lane; HOLD +
 *                         knob 1/2 = Pan L / Pan R); hardware Capture
 *                         button = retro grab
 *   Knobs 1-8             FX Density, Order, Loop Len, Slice Res,
 *                         Wet, Pitch Range, A/B, Seed
 *   Play button           passed through to Move (transport + clock
 *                         keep working under this UI)
 *
 * Talks to the DSP via host_module_set_param/get_param, which the shadow
 * UI shims to the slot-0 overtake DSP ("overtake_dsp:" params).
 * Screen reader: everything announced via shared/screen_reader.mjs.
 */

import {
    MoveKnob1, MoveCapture, MoveShift, MoveMainKnob, MoveLeft, MoveRight,
    Black, White, LightGrey, Red, BrightRed, Blue, Green, BrightGreen,
    Cyan, Purple, YellowGreen, OrangeRed
} from '/data/UserData/schwung/shared/constants.mjs';

import { decodeDelta, setLED, setButtonLED } from '/data/UserData/schwung/shared/input_filter.mjs';

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
const PAD_MONITOR = 73;   /* input monitoring on/off + feedback-guard override */
const PAD_MODE    = 74;   /* Stereo <-> Dual Mono */
const PAD_LANE    = 75;   /* dual mono: tap = lane L/R; hold + knob1/2 = pans */

/* Upper three pad rows: effect palette, grouped by family (not engine
 * order). Row 2: clean + stutter + tape. Row 3: reverse + pitch +
 * texture/shape + crush. Row 4: filters + space + destruction.
 * Top-right pad 99 = unlock (restore the seeded effect). */
const PAD_PALETTE_FIRST = 76;
const PAD_UNLOCK        = 99;
/* pad offset (0-22, rows bottom-up from pad 76) -> fx code. The factory
 * order below is the fallback; the live arrangement comes from the DSP's
 * `palette` param (rearranged in the web editor, persisted in presets). */
const PALETTE_DEFAULT = [
    0, 1, 8, 6, 5, 10, 11, 12,      /* clean, stutter reds, tape oranges */
    2, 9, 3, 4, 18, 13, 14, 7,      /* blues, purples, texture, crush */
    15, 16, 21, 17, 19, 22, 20      /* greens, cyans, dist */
];
/* entries {f, tok}: tok is the raw "f[:p[:p2[:m]]]" token — the pad UI
 * passes it straight through to set_slice/lock_slice/punch_fx, so pads
 * carry variant + depth + mix without this file understanding them */
let paletteLayout = PALETTE_DEFAULT.map(f => ({ f, tok: `${f}` }));
let paletteCsv = '';
function applyPaletteCsv(csv) {
    const v = csv.split(',').map(tok => {
        const f = parseInt(tok, 10);
        return isNaN(f) ? null : { f, tok };
    });
    if (v.length !== PALETTE_DEFAULT.length || v.some(e => e === null)) return;
    paletteLayout = v;
    paintPalette(false);
    needsRedraw = true;
}

const STEP_FIRST = 16;
const STEP_COUNT = 16;

/* Color / name / speech per fx code (matches smack_fx_t order in the DSP) */
const FX_COLORS = [
    0x10,        /* CLEAN      — dim white */
    Red,         /* RETRIG     — stutter family: reds */
    Blue,        /* REVERSE    — reverse family: blues */
    Purple,      /* PITCH      — pitch/speed family: purples */
    Purple,      /* SPEED */
    BrightRed,   /* GATE       — stutter family */
    BrightRed,   /* BUZZ */
    BrightGreen, /* CRUSH      — destruction family: bright green */
    Red,         /* REPEAT     — stutter family */
    Blue,        /* REVAFTER   — reverse family */
    OrangeRed,   /* TAPESTOP   — tape/scratch family: oranges */
    OrangeRed,   /* TAPESTART */
    OrangeRed,   /* SCRATCH */
    LightGrey,   /* ENV        — shape family: greys */
    LightGrey,   /* PAN */
    Green,       /* FILTER     — filter family: greens */
    Green,       /* VOWEL */
    Cyan,        /* TONALDELAY — space family: cyans */
    White,       /* FREEZE     — texture: white */
    Cyan,        /* DELAY */
    BrightGreen, /* DIST       — destruction family */
    YellowGreen, /* PHASER     — modulation: yellow-green */
    Cyan,        /* VERB       — space family */
    Purple,      /* PSHIFT     — pitch family */
    YellowGreen, /* RINGMOD    — modulation family */
    Green,       /* COMB       — filter family */
    White        /* SCATTER    — texture family */
];

const FX_NAMES = [
    'Clean', 'Retrig', 'Reverse', 'Pitch', 'Speed', 'Gate', 'Buzz',
    'Crush', 'Repeat', 'RevAfter', 'TapeStop', 'TapeStrt', 'Scratch',
    'Envelope', 'Pan', 'Filter', 'Vowel', 'TonalDly', 'Freeze', 'Delay',
    'Distort', 'Phaser', 'Reverb', 'PShift', 'RingMod', 'Comb', 'Scatter'
];

const FX_SPEECH = [
    'clean', 'retrigger', 'reverse', 'pitch', 'speed', 'gate', 'buzz',
    'bitcrush', 'repeat', 'reverse after', 'tape stop', 'tape start',
    'scratch', 'envelope', 'pan', 'filter', 'vowel', 'tonal delay',
    'freeze', 'delay', 'distortion', 'phaser', 'reverb',
    'pitch shift', 'ring mod', 'comb filter', 'scatter'
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
    { key: 'pitch_range',   name: 'Pit',  min: 1, max: 24,  step: 1,
      speech: 'Pitch Range', unit: ' semitones' },
    { key: 'ab',            name: 'A/B',  opts: ['A', 'B'],
      speech: 'A B', speechOpts: ['A clean', 'B pattern'] },
    { key: 'seed',          name: 'Seed', min: 1, max: 9999, step: 1,
      speech: 'Seed' }
];

let knobValues = [50, 0, 4, 1, 100, 12, 1, 4303];
let seedTurnAt = 0;
let seedTurnDirection = 0;

/* Seed spans 1..9999: slow turns stay exact, while fast turns accelerate
 * 1 -> 10 -> 50 -> 250 seeds per tick. Reversing starts fine again. */
function accelerateSeedDelta(delta) {
    const now = Date.now();
    const direction = delta > 0 ? 1 : -1;
    const elapsed = seedTurnAt > 0 ? now - seedTurnAt : 9999;
    let multiplier = 1;
    if (direction === seedTurnDirection) {
        if (elapsed <= 35) multiplier = 250;
        else if (elapsed <= 90) multiplier = 50;
        else if (elapsed <= 180) multiplier = 10;
    }
    seedTurnAt = now;
    seedTurnDirection = direction;
    return delta * multiplier;
}

let state = 0;          /* 0 idle, 1 armed, 2 rec, 3 looping */
let ab = 1;
let pattern = '';
let patternR = '';
let lockedMask = '';
let lockedMaskR = '';
let nSlices = 0;
let playSlice = -1;
let selectedSlice = -1;
let stepPage = 0;       /* 16-step window into loops with > 16 slices */
let tickCount = 0;
let needsRedraw = true;
let dspReady = false;

/* Dual mono */
let chanMode = 0;       /* 0 stereo, 1 dual mono */
let editLane = 0;       /* 0 = left lane, 1 = right lane */
let panL = 0, panR = 100;
let lanePadHeld = false;
let laneHoldUsed = false; /* a pan knob moved during the hold */

/* Hold Re-Roll >= this long: unlock every pinned slice and roll fresh */
const REROLL_HOLD_MS = 600;
/* BPM detection: Shift+Capture analyses the last 8 s of input */
let shiftHeld = false;
let detecting = false;
let bpmOverride = 0;
let rerollHeldAt = 0;      /* Date.now() at press, 0 = not held */
let rerollHoldFired = false;
/* Palette gesture: tap = set the effect (soft, next re-roll replaces it);
 * hold >= PIN_HOLD_MS = pin it (survives re-roll). */
const PIN_HOLD_MS = 600;
/* A/B pad: tap = toggle; hold >= AB_HOLD_MS = momentary punch (restore
 * the previous side on release) */
const AB_HOLD_MS = 350;
let abHeld = null;         /* { at, prev } */
let paletteHeld = null;    /* { pad, code, slice, at, fired } */

/* Global punch: palette pad held with NO slice selected forces that one
 * effect over the whole loop; aftertouch bends its parameter. These were
 * accidentally never declared — in a strict-mode module every read threw
 * ReferenceError, which the old host swallowed but schwung main treats as
 * fatal (exits the tool on any pad release). */
let punchPad = -1;             /* pad note currently punching, -1 = none */
let punchLastPressure = -1;    /* last aftertouch sent (throttle: Δ >= 3) */

/* Feedback guard: the host's slot guard never sees overtake modules, so we
 * mirror it here — speakers on + no line-in cable = the internal mic would
 * feed the speakers. Mute the DSP's input monitoring (ring keeps recording,
 * loop playback stays audible) until it's safe or the user overrides. */
let monitorOn = true;
let guardMuted = false;   /* we muted because of risk */
let guardOverride = false; /* user forced monitoring on during risk */


/* Knob page 2 — held-Shift layer. Same knobs, second param set. */
const KNOBS2 = [
    { key: 'pan_l',        name: 'PanL', min: 0, max: 100, step: 5,
      speech: 'Pan left lane' },
    { key: 'pan_r',        name: 'PanR', min: 0, max: 100, step: 5,
      speech: 'Pan right lane' },
    { key: 'channel_mode', name: 'Chan', opts: ['St', 'Dual'],
      speech: 'Channels', speechOpts: ['stereo', 'dual mono'] },
    { key: 'transport',    name: 'Trns', opts: ['Flw', 'Free'],
      speech: 'Transport', speechOpts: ['follow', 'free'] },
    { key: 'bpm_override', name: 'BPM',  min: 49, max: 200, step: 1,
      speech: 'BPM override', isBpm: true },
    { key: 'monitor',      name: 'Mon',  opts: ['Mute', 'On'],
      speech: 'Monitoring', speechOpts: ['muted', 'on'] },
    null,
    null
];
let knob2Values = [0, 100, 0, 0, 0, 1, 0, 0];

function knob2Display(i) {
    const k = KNOBS2[i];
    if (!k) return '';
    if (k.isBpm) return knob2Values[i] > 0 ? `${Math.round(knob2Values[i])}` : 'Off';
    if (k.opts) {
        const idx = Math.max(0, Math.min(k.opts.length - 1, Math.round(knob2Values[i])));
        return k.opts[idx];
    }
    return `${Math.round(knob2Values[i])}`;
}

function fetchKnob2() {
    for (let i = 0; i < KNOBS2.length; i++) {
        if (!KNOBS2[i]) continue;
        const v = gp(KNOBS2[i].key);
        if (v !== null) knob2Values[i] = parseFloat(v) || 0;
    }
}

function adjustKnob2(i, delta) {
    const k = KNOBS2[i];
    if (!k) return;
    let v;
    if (k.isBpm) {
        /* 49 and below = Off (project tempo); 50-200 = override */
        const cur = knob2Values[i] > 0 ? knob2Values[i] : 49;
        v = Math.max(49, Math.min(200, Math.round(cur) + delta * k.step));
        const out = v < 50 ? 0 : v;
        knob2Values[i] = out;
        bpmOverride = out;
        host_module_set_param(k.key, `${out}`);
        announceParameter(k.speech, out > 0 ? `${out}` : 'off, project tempo');
        needsRedraw = true;
        return;
    }
    const max = k.opts ? k.opts.length - 1 : k.max;
    const min = k.opts ? 0 : k.min;
    const step = k.opts ? 1 : k.step;
    v = Math.max(min, Math.min(max, knob2Values[i] + delta * step));
    if (v === knob2Values[i]) return;
    knob2Values[i] = v;
    host_module_set_param(k.key, `${Math.round(v)}`);
    if (k.speechOpts) announceParameter(k.speech, k.speechOpts[Math.round(v)]);
    else announceParameter(k.speech, `${Math.round(v)}`);
    /* keep the pad/guard state in sync with knob-driven changes */
    if (k.key === 'channel_mode') {
        chanMode = Math.round(v);
        if (!chanMode) editLane = 0;
    }
    if (k.key === 'monitor') monitorOn = Math.round(v) !== 0;
    if (k.key === 'pan_l') panL = Math.round(v);
    if (k.key === 'pan_r') panR = Math.round(v);
    syncPage2Paint();
    needsRedraw = true;
}

function syncPage2Paint() {
    paintTransport(false);
    paintSteps(false);
}

function startDetect() {
    host_module_set_param('detect_bpm', '1');
    detecting = true;
    announce('Detecting tempo');
    needsRedraw = true;
}

function feedbackRisk() {
    if (typeof host_speaker_active !== 'function') return false;
    if (typeof host_line_in_connected !== 'function') return false;
    return host_speaker_active() && !host_line_in_connected();
}

function setMonitor(on) {
    monitorOn = on;
    host_module_set_param('monitor', on ? '1' : '0');
    paintTransport(false);
    needsRedraw = true;
}

function reconcileFeedbackGuard() {
    const risk = feedbackRisk();
    if (risk && monitorOn && !guardOverride) {
        guardMuted = true;
        setMonitor(false);
        announce('Feedback risk. Input muted. Monitor pad to override, or plug in headphones.');
    } else if (!risk && guardMuted) {
        guardMuted = false;
        guardOverride = false;
        setMonitor(true);
        announce('Monitoring restored');
    }
}

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
    patternR = gp('pattern_r') || '';
    lockedMask = gp('locked') || '';
    lockedMaskR = gp('locked_r') || '';
    nSlices = parseInt(gp('n_slices') || '0');
    chanMode = parseInt(gp('channel_mode') || '0');
    panL = parseInt(gp('pan_l') || '0');
    panR = parseInt(gp('pan_r') || '100');
    monitorOn = (gp('monitor') || '1') !== '0';
    bpmOverride = parseFloat(gp('bpm_override')) || 0;
    const pv = gp('palette');
    if (pv !== null && pv !== paletteCsv) { paletteCsv = pv; applyPaletteCsv(pv); }
    fetchKnob2();
    return true;
}

/* pattern of the lane currently being edited */
function editPattern() {
    return (chanMode && editLane) ? patternR : pattern;
}

function lockKey(i) {
    return (chanMode && editLane) ? `lock_slice_r_${i}` : `lock_slice_${i}`;
}

function softKey(i) {
    return (chanMode && editLane) ? `set_slice_r_${i}` : `set_slice_${i}`;
}

function laneSpeech() {
    return editLane ? 'right lane' : 'left lane';
}

function stepPages() {
    return Math.max(1, Math.ceil(nSlices / STEP_COUNT));
}

function stepPageBy(delta) {
    const pages = stepPages();
    if (pages <= 1) return;
    const p = Math.max(0, Math.min(pages - 1, stepPage + (delta > 0 ? 1 : -1)));
    if (p === stepPage) return;
    stepPage = p;
    const base = stepPage * STEP_COUNT;
    announce(`Steps ${base + 1} to ${Math.min(base + STEP_COUNT, nSlices)}`);
    paintSteps(false);
    needsRedraw = true;
}

function sliceLocked(i) {
    const m = (chanMode && editLane) ? lockedMaskR : lockedMask;
    return m.charAt(i) === '1';
}

function sliceFx(i) {
    const pat = editPattern();
    if (i < 0 || i >= pat.length) return 0;
    const code = pat.charCodeAt(i) - 48;
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
    if (k.key === 'seed') delta = accelerateSeedDelta(delta);
    let v = Math.max(min, Math.min(max, knobValues[i] + delta * step));
    if (v === knobValues[i]) return;
    knobValues[i] = v;
    host_module_set_param(k.key, `${Math.round(v)}`);
    if (k.key === 'ab') ab = Math.round(v);
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
    setLED(PAD_MONITOR, monitorOn ? Green : BrightRed, force);
    setLED(PAD_MODE, chanMode ? White : 0x10, force);
    setLED(PAD_LANE, chanMode ? (editLane ? OrangeRed : Cyan) : Black, force);
}

function paintPalette(force) {
    for (let i = 0; i < paletteLayout.length; i++)
        setLED(PAD_PALETTE_FIRST + i, FX_COLORS[paletteLayout[i].f], force);
    setLED(PAD_UNLOCK, 0x2D /* dim blue: unlock */, force);
}

function paintSteps(force) {
    const pages = stepPages();
    setButtonLED(MoveLeft, (state === 3 && stepPage > 0) ? 1 : 0, force);
    setButtonLED(MoveRight, (state === 3 && stepPage < pages - 1) ? 1 : 0, force);
    const base = stepPage * STEP_COUNT;
    for (let i = 0; i < STEP_COUNT; i++) {
        const s = base + i;
        let color = Black;
        if (state === 3 && s < nSlices) {
            if (shiftHeld) {
                /* page-2 view: light only the pinned slices */
                color = sliceLocked(s) ? FX_COLORS[sliceFx(s)] : Black;
            } else {
                color = FX_COLORS[sliceFx(s)];
                /* B side shows the pattern; A side shows all-clean */
                if (!ab) color = FX_COLORS[0];
                if (s === selectedSlice) color = BrightGreen;  /* editing target */
                if (s === playSlice) color = White;            /* playhead wins */
            }
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
    if (detecting) title += ' @...';
    else if (bpmOverride > 0) title += ` @${Math.round(bpmOverride)}`;
    drawHeader(title);

    /* selected-slice line (lane-prefixed in dual mono, pin marker) */
    const lanePfx = chanMode ? (editLane ? 'R ' : 'L ') : '';
    if (state === 3 && selectedSlice >= 0 && selectedSlice < nSlices) {
        const pin = sliceLocked(selectedSlice) ? ' *pin' : '';
        print(2, 13, `${lanePfx}Step ${selectedSlice + 1}: ${FX_NAMES[sliceFx(selectedSlice)]}${pin}`, 1);
    } else if (state === 3) {
        print(2, 13, lanePfx + 'Press a step to edit', 1);
    } else {
        print(2, 13, 'Capture or Arm a loop', 1);
    }

    /* two rows of four knob params (Shift = page 2) */
    for (let i = 0; i < 8; i++) {
        const col = i % 4, row = (i / 4) | 0;
        const x = 2 + col * 32;
        const y = 24 + row * 17;
        if (shiftHeld) {
            if (KNOBS2[i]) {
                print(x, y, KNOBS2[i].name, 1);
                print(x, y + 8, knob2Display(i), 1);
            }
        } else {
            print(x, y, KNOBS[i].name, 1);
            print(x, y + 8, knobDisplay(i), 1);
        }
    }

    /* footer budget is ~20 chars total (128 px, no overlap guard in the
     * shared helper) \u2014 show one context-relevant hint set at a time */
    const pages = stepPages();
    let fLeft, fRight;
    if (shiftHeld) {
        fLeft = 'Knobs pg2 \u00b7 pins';
        fRight = '';
    } else if (state !== 3) {
        fLeft = '';
        fRight = 'Back:hide';
    } else {
        fLeft = 'Stp:sel Pad:fx';
        fRight = pages > 1 ? `p${stepPage + 1}/${pages}` : '';
    }
    drawFooter({ left: fLeft, right: fRight });
    needsRedraw = false;
}

/* ---- Actions ---- */

function assignFx(code, tok) {
    if (state !== 3 || selectedSlice < 0 || selectedSlice >= nSlices) {
        announce('Select a step first');
        return;
    }
    host_module_set_param(softKey(selectedSlice), tok || `${code}`);
    const where = chanMode ? `, ${laneSpeech()}` : '';
    announce(`Slice ${selectedSlice + 1}, ${FX_SPEECH[code]}${where}`);
    refreshSoon();
}

function unlockSlice() {
    if (state !== 3 || selectedSlice < 0 || selectedSlice >= nSlices) {
        announce('Select a step first');
        return;
    }
    host_module_set_param(lockKey(selectedSlice), '-1');
    announce(`Slice ${selectedSlice + 1} unlocked`);
    refreshSoon();
}

function adjustPan(which, delta) {
    let v = which ? panR : panL;
    v = Math.max(0, Math.min(100, v + delta * 5));
    if (which) { panR = v; host_module_set_param('pan_r', `${v}`); }
    else       { panL = v; host_module_set_param('pan_l', `${v}`); }
    announceParameter(which ? 'Pan right lane' : 'Pan left lane', `${v}`);
    needsRedraw = true;
}

/* ---- Lifecycle ---- */

globalThis.init = function() {
    dspReady = fetchAll();
    /* Overtake pads are EDITORS (mute/pin/palette) — turn the engine's
     * MIDI-note slice triggering off so any note stream that reaches the
     * DSP alongside our pad handling can't double-trigger cells. Slot-mode
     * smack-in (no custom pad UI) is where pad_play earns its keep. */
    if (dspReady) host_module_set_param('pad_play', '0');
    playSlice = -1;
    selectedSlice = -1;
    paintAll(true);
    needsRedraw = true;
    if (dspReady) {
        let spoken = 'Oversmack, ' + STATE_SPEECH[state];
        if (state === 3) spoken += ab ? ', side B' : ', side A';
        announceView(spoken);
        reconcileFeedbackGuard();
    }
};

let resumeRepaints = 0;   /* extra forced repaints after resume: a single
                             paintAll(true) is ~56 LED writes in one tick,
                             and the shim's LED queue can drop under that
                             burst — re-force a few times, spaced out */

globalThis.onResume = function() {
    /* LEDs were cleared while suspended: full forced repaint */
    dspReady = fetchAll();
    paintAll(true);
    resumeRepaints = 3;
    needsRedraw = true;
    announceView('Oversmack');
    reconcileFeedbackGuard();
};

globalThis.tick = function() {
    tickCount++;

    if (!dspReady) {
        dspReady = fetchAll();
        if (dspReady) {
            paintAll(true);
            needsRedraw = true;
            announceView('Oversmack, ' + STATE_SPEECH[state]);
            reconcileFeedbackGuard();
        } else if (needsRedraw) {
            drawUI();
        }
        return;
    }

    /* jack state can change mid-session (headphones unplugged) — re-check
     * the feedback guard about twice a second */
    if (tickCount % 15 === 0) reconcileFeedbackGuard();

    /* spaced post-resume repaints heal any LED writes the queue dropped */
    if (resumeRepaints > 0 && tickCount % 8 === 0) {
        paintAll(true);
        needsRedraw = true;
        resumeRepaints--;
    }

    /* poll the async BPM detection result */
    if (detecting && tickCount % 6 === 0) {
        const v = parseFloat(gp('detected_bpm') || '-1');
        if (v >= 0) {
            detecting = false;
            if (v > 0) {
                bpmOverride = v;
                announce(`Detected ${Math.round(v)} BPM`);
            } else {
                announce('No clear tempo found');
            }
            needsRedraw = true;
        }
    }

    /* palette pad held long enough: upgrade the soft assign to a pin */
    if (paletteHeld && !paletteHeld.fired &&
        Date.now() - paletteHeld.at >= PIN_HOLD_MS) {
        paletteHeld.fired = true;
        host_module_set_param(
            (chanMode && editLane) ? `lock_slice_r_${paletteHeld.slice}`
                                   : `lock_slice_${paletteHeld.slice}`,
            paletteHeld.tok);
        announce(`Slice ${paletteHeld.slice + 1} pinned`);
        refreshSoon();
    }

    /* Re-Roll held long enough: clear every pin and roll fresh */
    if (rerollHeldAt && !rerollHoldFired &&
        Date.now() - rerollHeldAt >= REROLL_HOLD_MS) {
        rerollHoldFired = true;
        host_module_set_param('unlock_all', '1');
        selectedSlice = -1;
        announce('Fresh roll, all pins cleared');
        refreshSoon();
    }

    /* playhead chase: cheap single get_param per tick */
    if (state === 3) {
        const ps = parseInt(gp('play_slice') || '-1');
        if (ps !== playSlice) {
            playSlice = ps;
            paintSteps(false);
        }
    }

    /* periodic full refresh — device knob edits, quantized AB flips, AND
     * changes arriving from the web editor: knob values must re-poll too,
     * or the Move screen shows stale numbers after a browser-side change */
    if (tickCount % 12 === 0) {
        const oldState = state, oldAb = ab, oldPattern = pattern, oldPatternR = patternR;
        const oldKnobs = knobValues.join(','), oldMon = monitorOn;
        fetchAll();
        if (state !== 3) { selectedSlice = -1; stepPage = 0; }
        if (stepPage >= stepPages()) stepPage = stepPages() - 1;
        /* speak async transitions (armed -> recording -> looping); A/B is
         * announced at press time (applied value lags a quantized flip) */
        if (state !== oldState)
            announce(state === 3 ? `looping, ${nSlices} slices` : STATE_SPEECH[state]);
        if (state !== oldState || ab !== oldAb ||
            pattern !== oldPattern || patternR !== oldPatternR) {
            paintTransport(false);
            paintSteps(false);
            needsRedraw = true;
        }
        if (knobValues.join(',') !== oldKnobs) needsRedraw = true;
        if (monitorOn !== oldMon) { paintTransport(false); needsRedraw = true; }
    }

    if (needsRedraw) drawUI();
};

globalThis.onMidiMessageInternal = function(data) {
    const status = data[0] & 0xF0;
    const d1 = data[1];
    const d2 = data[2];

    /* pad pressure (poly aftertouch) bends the punched effect */
    if (status === 0xA0) {
        if (punchPad >= 0 && d1 === punchPad &&
            Math.abs(d2 - punchLastPressure) >= 3) {
            punchLastPressure = d2;
            host_module_set_param('punch_pressure', `${d2}`);
        }
        return;
    }

    if (status === 0xB0) {
        if (d1 === MoveShift) {
            const was = shiftHeld;
            shiftHeld = d2 >= 64;
            if (was !== shiftHeld) { fetchKnob2(); paintSteps(false); needsRedraw = true; }
            return;
        }
        /* Capture button = retro grab. Shift+Capture belongs to the
         * host's skipback — do nothing so the two don't double-fire. */
        if (d1 === MoveCapture && d2 >= 64) {
            if (shiftHeld) return;
            host_module_set_param('capture', '1');
            announce('Capture');
            refreshSoon();
            return;
        }
        /* Jog wheel or arrow buttons page through the steps (bars) */
        if (d1 === MoveMainKnob) {
            const delta = decodeDelta(d2);
            if (delta !== 0 && state === 3) stepPageBy(delta);
            return;
        }
        if (d1 === MoveLeft && d2 >= 64) {
            if (state === 3) stepPageBy(-1);
            return;
        }
        if (d1 === MoveRight && d2 >= 64) {
            if (state === 3) stepPageBy(1);
            return;
        }
        /* Knobs 1-8; while the lane pad is held, knobs 1-2 are the pans */
        if (d1 >= MoveKnob1 && d1 < MoveKnob1 + 8) {
            const delta = decodeDelta(d2);
            if (delta === 0) return;
            const k = d1 - MoveKnob1;
            if (lanePadHeld && chanMode && k < 2) {
                laneHoldUsed = true;
                adjustPan(k, delta);
            } else if (shiftHeld) {
                adjustKnob2(k, delta);
            } else {
                adjustKnob(k, delta);
            }
            return;
        }
        return;
    }

    /* palette pad release: end a global punch */
    if ((status === 0x80 || (status === 0x90 && d2 === 0)) && d1 === punchPad) {
        punchPad = -1;
        host_module_set_param('punch_fx', '-1');
        needsRedraw = true;
        return;
    }

    /* palette pad release ends the pin-hold window */
    if ((status === 0x80 || (status === 0x90 && d2 === 0)) &&
        paletteHeld && d1 === paletteHeld.pad) {
        paletteHeld = null;
        return;
    }

    /* A/B pad release: a long hold was a momentary punch — snap back */
    if ((status === 0x80 || (status === 0x90 && d2 === 0)) && d1 === PAD_AB) {
        if (abHeld && Date.now() - abHeld.at >= AB_HOLD_MS && ab !== abHeld.prev) {
            ab = abHeld.prev;
            host_module_set_param('ab', `${ab}`);
            announce(ab ? 'B, pattern' : 'A, clean loop');
            refreshSoon();
        }
        abHeld = null;
        return;
    }

    /* re-roll pad release ends the hold window */
    if ((status === 0x80 || (status === 0x90 && d2 === 0)) && d1 === PAD_REROLL) {
        rerollHeldAt = 0;
        rerollHoldFired = false;
        return;
    }

    /* lane pad release: a plain tap (no pan knob turned) switches lanes */
    if ((status === 0x80 || (status === 0x90 && d2 === 0)) && d1 === PAD_LANE) {
        if (lanePadHeld && !laneHoldUsed && chanMode) {
            editLane = editLane ? 0 : 1;
            selectedSlice = -1;
            announce('Editing ' + laneSpeech());
            paintTransport(false);
            paintSteps(false);
            needsRedraw = true;
        }
        lanePadHeld = false;
        laneHoldUsed = false;
        return;
    }

    if (status === 0x90 && d2 > 0) {
        /* Transport pads */
        if (d1 === PAD_CAPTURE) {
            host_module_set_param('capture', '1');
            announce('Capture');
            refreshSoon();
            return;
        }
        if (d1 === PAD_ARM)     { host_module_set_param('arm', '1');     announce('Arm');     refreshSoon(); return; }
        if (d1 === PAD_REROLL) {
            /* Shift+Re-Roll = detect BPM (Shift+Capture is the host's
             * skipback); tap: re-roll; hold: unlock everything + fresh */
            if (shiftHeld) { startDetect(); return; }
            host_module_set_param('reroll', '1');
            announce('Re-roll');
            rerollHeldAt = Date.now();
            rerollHoldFired = false;
            refreshSoon();
            return;
        }
        if (d1 === PAD_CLEAR)   { host_module_set_param('clear', '1');   announce('Clear');   refreshSoon(); return; }
        if (d1 === PAD_AB) {
            abHeld = { at: Date.now(), prev: ab };
            ab = ab ? 0 : 1;
            host_module_set_param('ab', `${ab}`);
            announce(ab ? 'B, pattern' : 'A, clean loop');
            paintTransport(false);
            paintSteps(false);
            refreshSoon();
            return;
        }
        if (d1 === PAD_MONITOR) {
            if (monitorOn) {
                guardMuted = false;
                guardOverride = false;
                setMonitor(false);
                announce('Input muted');
            } else {
                guardMuted = false;
                if (feedbackRisk()) {
                    guardOverride = true;
                    announce('Monitoring on. Feedback risk!');
                } else {
                    announce('Monitoring on');
                }
                setMonitor(true);
            }
            return;
        }
        if (d1 === PAD_MODE) {
            chanMode = chanMode ? 0 : 1;
            host_module_set_param('channel_mode', `${chanMode}`);
            if (!chanMode) editLane = 0;
            announce(chanMode ? 'Dual mono' : 'Stereo');
            paintTransport(false);
            paintSteps(false);
            refreshSoon();
            return;
        }
        if (d1 === PAD_LANE) {
            if (chanMode) {
                lanePadHeld = true;
                laneHoldUsed = false;
            } else {
                announce('Stereo mode. Pad 7 switches to dual mono');
            }
            return;
        }

        /* Effect palette. With a step selected: tap = set (soft), hold =
         * pin. With NO selection: momentary global punch — the whole loop
         * plays this one effect until release; pressure bends it. */
        if (d1 === PAD_UNLOCK) { unlockSlice(); return; }
        if (d1 >= PAD_PALETTE_FIRST && d1 < PAD_PALETTE_FIRST + paletteLayout.length) {
            const ent = paletteLayout[d1 - PAD_PALETTE_FIRST];
            const code = ent.f;
            if (state === 3 && selectedSlice < 0) {
                punchPad = d1;
                punchLastPressure = -1;
                host_module_set_param('punch_fx', ent.tok);
                announce(`${FX_SPEECH[code]} punch`);
                needsRedraw = true;
                return;
            }
            const slice = selectedSlice;
            assignFx(code, ent.tok);
            if (state === 3 && slice >= 0 && slice < nSlices) {
                paletteHeld = { pad: d1, code, tok: ent.tok,
                                slice, at: Date.now(), fired: false };
            }
            return;
        }

        /* Step press: select a slice for editing; same step again deselects */
        if (d1 >= STEP_FIRST && d1 < STEP_FIRST + STEP_COUNT) {
            const i = stepPage * STEP_COUNT + (d1 - STEP_FIRST);
            if (state === 3 && i < nSlices) {
                if (selectedSlice === i) {
                    selectedSlice = -1;
                    announce('Selection cleared');
                } else {
                    selectedSlice = i;
                    const where = chanMode ? `, ${laneSpeech()}` : '';
                    const pin = sliceLocked(i) ? ', pinned' : '';
                    announce(`Slice ${i + 1}, ${FX_SPEECH[sliceFx(i)]}${pin}${where}`);
                }
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
