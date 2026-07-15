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
 *   Pad 74                Stereo <-> Dual Mono toggle
 *   Pad 75                dual mono: tap = edit L or R lane; HOLD +
 *                         knob 1/2 = Pan L / Pan R
 *   Capture button (CC52) same as pad 68
 *   Steps 1-16            pattern display (color = effect per slice);
 *                         press to mute a slice's effect, press again
 *                         to restore the seeded one
 *   Knobs 1-8             FX Density, Order, Loop Len, Slice Res,
 *                         Wet, Pitch Range, AB Quantize, Seed
 *
 * Screen reader: pad actions, knob changes and loop-state transitions are
 * announced via shared/screen_reader.mjs when the reader is enabled.
 */

import {
    MoveKnob1, MoveCapture, MoveShift, MoveMainButton, MoveMainKnob,
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


/* Transport pads (bottom-left corner of the 8x4 grid) */
const PAD_CAPTURE = 68;
const PAD_ARM     = 69;
const PAD_AB      = 70;
const PAD_REROLL  = 71;
const PAD_CLEAR   = 76;
const PAD_MODE    = 74;   /* Stereo <-> Dual Mono (same pads as oversmack) */
const PAD_LANE    = 75;   /* dual mono: tap = lane L/R; hold + knob1/2 = pans */
const PAD_MONITOR = 73;   /* smack-in only: input monitoring + feedback guard */
const PAD_PAGE    = 72;   /* next 16-step window (loops > 16 slices) */

/* Step buttons show the first 16 slices */
const STEP_FIRST = 16;
const STEP_COUNT = 16;

/* Slice LED color by effect code (matches smack_fx_t order in the DSP) */
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

const STATE_NAMES = ['IDLE', 'ARMED', 'REC', 'LOOP'];
const STATE_SPEECH = ['idle', 'armed', 'recording', 'looping'];

/* Knobs 1-8 (CC 71-78) left to right. `name`/`opts` are the terse screen
 * strings; `speech`/`speechOpts`/`unit` are what the screen reader says
 * (abbreviations like "½st" read as gibberish through TTS). */
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
let patternR = '';
let nSlices = 0;
let playSlice = -1;
let stepPage = 0;       /* 16-step window into loops with > 16 slices */
let tickCount = 0;
let needsRedraw = true;

/* Dual mono */
let chanMode = 0;       /* 0 stereo, 1 dual mono */
let editLane = 0;       /* 0 = left lane, 1 = right lane */
let panL = 0, panR = 100;
let lanePadHeld = false;
let laneHoldUsed = false;

/* Hold Re-Roll >= this long: unlock every pinned slice and roll fresh */
const REROLL_HOLD_MS = 600;
/* A/B pad: tap = toggle; hold >= AB_HOLD_MS = momentary punch (restore
 * the previous side on release) */
const AB_HOLD_MS = 350;
let abHeld = null;         /* { at, prev } */
/* BPM detection: Shift+Capture analyses the last 8 s of input */
let shiftHeld = false;
let detecting = false;
let bpmOverride = 0;
let rerollHeldAt = 0;
let rerollHoldFired = false;
let lockedMask = '';
let lockedMaskR = '';

/* Feedback guard — smack-in only (hw_input=1: the DSP reads the mic/line
 * input directly). The host's slot guard covers smack-in as a plain slot
 * synth but is blind to it INSIDE a chain patch, so while this editor is
 * open we mirror it: speakers on + no line-in cable -> mute the DSP's
 * input monitoring (ring keeps recording, loop stays audible). The smack
 * audio_fx build processes upstream chain audio; the guard never fires. */
let hwInput = false;
let monitorOn = true;
let guardMuted = false;
let guardOverride = false;


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
    { key: 'pad_play', name: 'Pads', opts: ['Off', 'On'],
      speech: 'Pad play', speechOpts: ['off', 'on'] },
    { key: 'pad_rate', name: 'Rate', opts: ['1/16', '1/8', '1/4', '1/2', '1br'],
      speech: 'Pad rate',
      speechOpts: ['sixteenth', 'eighth', 'quarter', 'half', '1 bar'] }
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
    updateTransportLEDs();
    updateStepLEDs();
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
    updateTransportLEDs();
    needsRedraw = true;
}

function reconcileFeedbackGuard() {
    if (!hwInput) return;
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
    for (let i = 0; i < KNOBS.length; i++) {
        const v = gp(KNOBS[i].key);
        if (v !== null) knobValues[i] = parseFloat(v) || 0;
    }
    state = parseInt(gp('run_state') || '0');
    ab = parseInt(gp('ab') || '1');
    pattern = gp('pattern') || '';
    patternR = gp('pattern_r') || '';
    lockedMask = gp('locked') || '';
    lockedMaskR = gp('locked_r') || '';
    nSlices = parseInt(gp('n_slices') || '0');
    chanMode = parseInt(gp('channel_mode') || '0');
    panL = parseInt(gp('pan_l') || '0');
    panR = parseInt(gp('pan_r') || '100');
    hwInput = gp('hw_input') === '1';
    monitorOn = (gp('monitor') || '1') !== '0';
    bpmOverride = parseFloat(gp('bpm_override')) || 0;
    fetchKnob2();
}

/* pattern / lock target of the lane currently being edited */
function editPattern() {
    return (chanMode && editLane) ? patternR : pattern;
}

function lockKey(i) {
    return (chanMode && editLane) ? `lock_slice_r_${i}` : `lock_slice_${i}`;
}

function laneSpeech() {
    return editLane ? 'right lane' : 'left lane';
}

function stepPages() {
    return Math.max(1, Math.ceil(nSlices / STEP_COUNT));
}

function adjustPan(which, delta) {
    let v = which ? panR : panL;
    v = Math.max(0, Math.min(100, v + delta * 5));
    if (which) { panR = v; host_module_set_param('pan_r', `${v}`); }
    else       { panL = v; host_module_set_param('pan_l', `${v}`); }
    announceParameter(which ? 'Pan right lane' : 'Pan left lane', `${v}`);
    needsRedraw = true;
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

function updateTransportLEDs() {
    setLED(PAD_CAPTURE, Green);
    setLED(PAD_ARM, state === 1 || state === 2 ? BrightRed : Red);
    setLED(PAD_AB, state === 3 ? (ab ? White : LightGrey) : Black);
    setLED(PAD_REROLL, Blue);
    setLED(PAD_CLEAR, state === 3 ? OrangeRed : Black);
    setLED(PAD_MODE, chanMode ? White : 0x10);
    setLED(PAD_LANE, chanMode ? (editLane ? OrangeRed : Cyan) : Black);
    setLED(PAD_MONITOR, hwInput ? (monitorOn ? Green : BrightRed) : Black);
}

function updateStepLEDs() {
    const base = stepPage * STEP_COUNT;
    const pat = editPattern();
    for (let i = 0; i < STEP_COUNT; i++) {
        const s = base + i;
        let color = Black;
        if (state === 3 && s < nSlices) {
            const code = pat.charCodeAt(s) - 48;
            const fxc = FX_COLORS[(code >= 0 && code < FX_COLORS.length) ? code : 0];
            if (shiftHeld) {
                /* page-2 view: light only the pinned slices */
                const msk = (chanMode && editLane) ? lockedMaskR : lockedMask;
                color = msk.charAt(s) === '1' ? fxc : Black;
            } else {
                color = fxc;
                /* B side shows the pattern; A side shows all-clean */
                if (!ab) color = FX_COLORS[0];
                if (s === playSlice) color = White;   /* playhead chase */
            }
        }
        setLED(STEP_FIRST + i, color);
    }
}

/* ---- Screen ---- */

function drawUI() {
    clear_screen();
    let title = 'SMACK  ' + STATE_NAMES[state];
    if (state === 3) title += ab ? ' · B' : ' · A';
    if (detecting) title += ' @...';
    else if (bpmOverride > 0) title += ` @${Math.round(bpmOverride)}`;
    drawHeader(title);

    /* two rows of four knob params (Shift = page 2) */
    for (let i = 0; i < 8; i++) {
        const col = i % 4, row = (i / 4) | 0;
        const x = 2 + col * 32;
        const y = 15 + row * 20;
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

    /* on-screen step grid: in a slot editor Move firmware keeps the pad
     * LEDs, so the SCREEN carries the pattern — 16 cells for the current
     * page in the free band between knob row 2 (ends y49) and the footer
     * rule (y55): filled = effected slice, hollow = clean, playhead = the
     * 1px bar underneath. Pin/fx counts live in the footer. */
    if (state === 3) {
        const pat = editPattern();
        const base = stepPage * STEP_COUNT;
        const gy = 50;
        for (let i = 0; i < STEP_COUNT; i++) {
            const s = base + i;
            if (s >= nSlices) break;
            const x = i * 8;
            if (pat[s] && pat[s] !== '0') fill_rect(x, gy, 7, 4, 1);
            else draw_rect(x, gy, 7, 4, 1);
            if (s === playSlice) fill_rect(x, gy + 4, 7, 1, 1);
        }
    }

    /* footer budget is ~20 chars total (128 px, no overlap guard in the
     * shared helper) \u2014 show one context-relevant hint set at a time */
    {
        const pages = stepPages();
        let fLeft, fRight;
        if (shiftHeld) {
            fLeft = 'Knobs pg2';
            fRight = 'click=swap';
        } else if (state !== 3) {
            fLeft = 'click=Cap';
            fRight = 'Shft=more';
        } else {
            /* pattern summary (the old y55 line collided with the rule) */
            const pat = editPattern();
            const msk = (chanMode && editLane) ? lockedMaskR : lockedMask;
            let fxCount = 0, pinCount = 0;
            for (let i = 0; i < pat.length; i++)
                if (pat[i] !== '0') fxCount++;
            for (let i = 0; i < msk.length; i++)
                if (msk[i] === '1') pinCount++;
            const lanePfx = chanMode ? (editLane ? 'R ' : 'L ') : '';
            const pins = pinCount ? ` ${pinCount}p` : '';
            fLeft = `${lanePfx}${nSlices}sl ${fxCount}fx${pins}`;
            fRight = pages > 1 ? `p${stepPage + 1}/${pages}` : '';
        }
        drawFooter({ left: fLeft, right: fRight });
    }
    needsRedraw = false;
}

/* ---- Lifecycle ---- */

function init() {
    fetchAll();
    playSlice = -1;
    updateTransportLEDs();
    updateStepLEDs();
    needsRedraw = true;
    let spoken = 'Smack, ' + STATE_SPEECH[state];
    if (state === 3) spoken += ab ? ', side B' : ', side A';
    announceView(spoken);
    reconcileFeedbackGuard();
}

function tick() {
    tickCount++;

    /* jack state can change mid-session — re-check the guard ~2x/second */
    if (hwInput && tickCount % 15 === 0) reconcileFeedbackGuard();

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

    /* Re-Roll held long enough: clear every pin and roll fresh */
    if (rerollHeldAt && !rerollHoldFired &&
        Date.now() - rerollHeldAt >= REROLL_HOLD_MS) {
        rerollHoldFired = true;
        host_module_set_param('unlock_all', '1');
        announce('Fresh roll, all pins cleared');
        refreshSoon();
    }

    /* playhead chase: cheap single get_param per tick. The screen grid
     * chases too (the pads belong to firmware in a slot editor). */
    if (state === 3) {
        const ps = parseInt(gp('play_slice') || '-1');
        if (ps !== playSlice) {
            playSlice = ps;
            updateStepLEDs();
            needsRedraw = true;
        }
    }

    /* periodic full refresh — device knob edits, quantized AB flips, AND
     * changes arriving from the web editor: knob values must re-poll too,
     * or the Move screen shows stale numbers after a browser-side change */
    if (tickCount % 12 === 0) {
        const oldState = state, oldAb = ab, oldPattern = pattern, oldPatternR = patternR;
        const oldKnobs = knobValues.join(','), oldMon = monitorOn;
        fetchAll();
        /* speak async transitions (armed -> recording -> looping) as they
         * land; A/B is announced at press time instead, because the applied
         * value lags the pad while a quantized flip is pending */
        if (state !== oldState)
            announce(state === 3 ? `looping, ${nSlices} slices` : STATE_SPEECH[state]);
        if (state !== 3) stepPage = 0;
        if (stepPage >= stepPages()) stepPage = stepPages() - 1;
        if (state !== oldState || ab !== oldAb ||
            pattern !== oldPattern || patternR !== oldPatternR) {
            updateTransportLEDs();
            updateStepLEDs();
            needsRedraw = true;
        }
        if (knobValues.join(',') !== oldKnobs) needsRedraw = true;
        if (monitorOn !== oldMon) { updateTransportLEDs(); needsRedraw = true; }
    }

    if (needsRedraw) drawUI();
}

function onMidiMessageInternal(data) {
    const status = data[0] & 0xF0;
    const d1 = data[1];
    const d2 = data[2];

    if (status === 0xB0) {
        if (d1 === MoveShift) {
            const was = shiftHeld;
            shiftHeld = d2 >= 64;
            if (was !== shiftHeld) { fetchKnob2(); updateStepLEDs(); needsRedraw = true; }
            return;
        }
        /* Jog-click = Capture, Shift+jog-click = swap module. Pads and
         * the hardware Capture button never reach a slot editor (Move
         * firmware keeps them), so the jog carries the transport here.
         * host_swap_module unloads this UI and opens the module chooser
         * — call it last, touch nothing after. */
        if (d1 === MoveMainButton && d2 > 0) {
            if (!shiftHeld) {
                host_module_set_param('capture', '1');
                announce('Capture');
                refreshSoon();
                return;
            }
            if (typeof host_swap_module === 'function') {
                announce('Module chooser');
                host_swap_module();
            } else {
                announce('Swap needs a newer schwung');
            }
            return;
        }
        /* Jog-turn = A/B: right lands on B (pattern), left on A (clean) */
        if (d1 === MoveMainKnob) {
            const delta = decodeDelta(d2);
            if (delta === 0 || state !== 3) return;
            const want = delta > 0 ? 1 : 0;
            if (want === ab) return;
            ab = want;
            host_module_set_param('ab', `${ab}`);
            announce(ab ? 'B, pattern' : 'A, clean loop');
            refreshSoon();
            return;
        }
        /* Capture button mirrors the capture pad. Shift+Capture belongs
         * to the host's skipback — do nothing so they don't double-fire. */
        if (d1 === MoveCapture && d2 >= 64) {
            if (shiftHeld) return;
            host_module_set_param('capture', '1');
            announce('Capture');
            refreshSoon();
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
            announce('Editing ' + laneSpeech());
            updateTransportLEDs();
            updateStepLEDs();
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
        if (d1 === PAD_ARM)     { host_module_set_param('arm', '1');     announce('Arm');      refreshSoon(); return; }
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
        if (d1 === PAD_CLEAR)   { host_module_set_param('clear', '1');   announce('Clear');    refreshSoon(); return; }
        if (d1 === PAD_AB) {
            abHeld = { at: Date.now(), prev: ab };
            ab = ab ? 0 : 1;
            host_module_set_param('ab', `${ab}`);
            announce(ab ? 'B, pattern' : 'A, clean loop');
            refreshSoon();
            return;
        }
        if (d1 === PAD_PAGE) {
            const pages = stepPages();
            if (state === 3 && pages > 1) {
                stepPage = (stepPage + 1) % pages;
                const base = stepPage * STEP_COUNT;
                announce(`Steps ${base + 1} to ${Math.min(base + STEP_COUNT, nSlices)}`);
                updateStepLEDs();
                needsRedraw = true;
            }
            return;
        }
        if (d1 === PAD_MONITOR) {
            if (!hwInput) return;   /* audio_fx build: pad is inert */
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
            updateTransportLEDs();
            refreshSoon();
            return;
        }
        if (d1 === PAD_LANE) {
            if (chanMode) {
                lanePadHeld = true;
                laneHoldUsed = false;
            } else {
                announce('Stereo mode. Pad above re-roll switches to dual mono');
            }
            return;
        }

        /* Step press: mute a slice's effect / restore the seeded one
         * (on the lane being edited, in dual mono) */
        if (d1 >= STEP_FIRST && d1 < STEP_FIRST + STEP_COUNT) {
            const i = stepPage * STEP_COUNT + (d1 - STEP_FIRST);
            if (state === 3 && i < nSlices) {
                const code = editPattern().charCodeAt(i) - 48;
                const where = chanMode ? `, ${laneSpeech()}` : '';
                if (code > 0) {
                    host_module_set_param(lockKey(i), '0');
                    announce(`Slice ${i + 1} muted${where}`);
                } else {
                    host_module_set_param(lockKey(i), '-1');
                    announce(`Slice ${i + 1} restored${where}`);
                }
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
