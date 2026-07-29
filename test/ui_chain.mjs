/*
 * Smack — chain UI harness.
 *
 * Measures, not just checks. host_module_get_param in a chain editor is
 * shimmed onto shadow_get_param: a BLOCKING round-trip to the shim, serviced
 * once per SPI frame (~23 ms) and abandoned after 100 ms. The channel serves
 * roughly 44 reads a second in total. Past that ceiling reads TIME OUT and
 * return null, and any code that folds null into a literal default then writes
 * that default back to the DSP on the next knob turn.
 *
 * That chain shipped three hardware bugs in the sibling module Work while its
 * 445-check engine suite stayed green — none of them were engine bugs. Chain
 * editors get no bulk-read path (shim_handle_param_bulk only serves overtake
 * DSPs), so the only fix here is to read less.
 *
 * Run via `make test`.
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = fs.readFileSync(path.join(root, 'src/ui_chain.js'), 'utf8');

const MoveKnob1 = 71, MoveShift = 49, MoveMainKnob = 14;
const MoveMainButton = 3, MoveCapture = 52;

const constants = {
    MoveKnob1, MoveCapture, MoveShift, MoveMainButton, MoveMainKnob,
    Black: 0, White: 120, LightGrey: 118, Red: 127, BrightRed: 1, Blue: 125,
    Green: 126, BrightGreen: 8, Cyan: 14, Purple: 22, YellowGreen: 30,
    OrangeRed: 2
};

const params = new Map([
    ['run_state', '3'],                    /* looping — the busiest state */
    ['ab', '1'], ['pattern', '1,2,3,4,5,6,7,8'], ['pattern_r', '1,2,3,4,5,6,7,8'],
    ['locked', ''], ['locked_r', ''], ['n_slices', '8'],
    ['channel_mode', '0'], ['pan_l', '0'], ['pan_r', '100'],
    ['hw_input', '0'], ['monitor', '1'], ['bpm_override', '0'],
    ['detected_bpm', '-1'], ['play_slice', '0'],
    ['fx_density', '50'], ['order_density', '50'], ['loop_len', '4'],
    ['slice_res', '1'], ['wet', '100'], ['reroll', '0'], ['seed', '1000']
]);

let roundTrips = 0;
let readFailures = 0;
const announcements = [];
const writes = [];

const context = vm.createContext({
    console, Math, Number, JSON, String, Array, parseInt, parseFloat, isFinite, Date,
    clear_screen() {}, print() {}, fill_rect() {}, draw_rect() {},
    text_width(t) { return String(t).length * 6; },
    move_midi_internal_send() {},
    host_speaker_active() { return false; },
    host_line_in_connected() { return true; },
    host_swap_module() {},
    host_module_get_param(key) {
        roundTrips++;
        if (readFailures > 0) { readFailures--; return null; }
        return params.get(key) ?? '0';
    },
    host_module_set_param(key, value) {
        writes.push({ key, value: String(value) });
        params.set(key, String(value));
    }
});

function synthetic(exports) {
    return new vm.SyntheticModule(Object.keys(exports), function initialize() {
        for (const [name, value] of Object.entries(exports)) this.setExport(name, value);
    }, { context });
}

const modules = new Map([
    ['/data/UserData/schwung/shared/constants.mjs', synthetic(constants)],
    ['/data/UserData/schwung/shared/input_filter.mjs', synthetic({
        /* Transcribed from schwung's src/shared/input_filter.mjs — decodeDelta
         * returns the ACCUMULATED tick count, which is exactly why the knob
         * maths needs a cap. Do not simplify. */
        decodeDelta(v) {
            if (v === 0) return 0;
            if (v >= 1 && v <= 63) return v;
            if (v >= 65 && v <= 127) return -(128 - v);
            return 0;
        },
        setLED() {}, setButtonLED() {}
    })],
    ['/data/UserData/schwung/shared/menu_layout.mjs', synthetic({
        drawMenuHeader() {}, drawMenuFooter() {}
    })],
    ['/data/UserData/schwung/shared/screen_reader.mjs', synthetic({
        announce(t) { announcements.push(String(t)); },
        announceParameter(l, v) { announcements.push(`${l} ${v}`); },
        announceView(t) { announcements.push(String(t)); }
    })]
]);

const module_ = new vm.SourceTextModule(source, { context, identifier: 'ui_chain.js' });
await module_.link((specifier) => {
    const found = modules.get(specifier);
    assert(found, `unexpected import: ${specifier}`);
    return found;
});
await module_.evaluate();

/* The chain UI publishes its entry points on a namespace rather than globals. */
const ui = context.chain_ui ?? context;
const cc = (num, value) => ui.onMidiMessageInternal([0xb0, num, value]);
const settle = (n = 20) => { for (let i = 0; i < n; i++) ui.tick(); };

/* ------------------------------------------------------------------ tests */

ui.init();
settle(40);

/* The headline measurement, taken in the LOOPING state because that is when
 * the playhead chase runs. */
roundTrips = 0;
settle(44);                                    /* one second of ticks */
assert(roundTrips <= 35,
    `steady state costs ${roundTrips} blocking round-trips per second against a ` +
    'channel that serves about 44 — at that rate the editor starves itself and ' +
    'reads start timing out');

/* Turning a knob must not stall behind a refresh. */
roundTrips = 0;
cc(MoveKnob1, 1);
assert(roundTrips === 0,
    `a knob detent cost ${roundTrips} blocking round-trips on the input path`);

/* decodeDelta is accumulated. Loop Length is a nine-entry enum, so a raw delta
 * pins it to an end and the middle options are unreachable by a normal turn. */
params.set('loop_len', '4');
ui.init();
settle(30);
writes.length = 0;
cc(MoveKnob1 + 2, 1);                          /* knob 3 = Loop Length */
let w = writes.find(x => x.key === 'loop_len');
assert.equal(w?.value, '5', `one detent moved loop_len to ${w?.value}, expected 5`);

params.set('loop_len', '4');
ui.init();
settle(30);
writes.length = 0;
cc(MoveKnob1 + 2, 30);                         /* a fast spin */
w = writes.find(x => x.key === 'loop_len');
assert(w && Number(w.value) > 4 && Number(w.value) < 8,
    `a fast spin drove loop_len to ${w?.value} of 0-8; it must move a chunk, ` +
    'not straight to the end stop');

/* The Seed knob's ramp is DELIBERATE — a 250x multiplier on a sustained turn,
 * because the seed space is huge and browsing it one integer at a time is
 * useless. The cap must not flatten it. */
params.set('seed', '1000');
ui.init();
settle(30);
writes.length = 0;
cc(MoveKnob1 + 7, 20);                         /* knob 8 = Seed */
w = writes.find(x => x.key === 'seed');
assert(w && Math.abs(Number(w.value) - 1000) >= 20,
    `Seed moved only to ${w?.value} from 1000 — its acceleration is intentional ` +
    'and must survive the short-range cap');

/* A dead param channel must not rewrite the engine with defaults. */
params.set('fx_density', '75');
params.set('wet', '60');
ui.init();
settle(30);
readFailures = 600;
settle(60);
readFailures = 0;
assert.equal(params.get('fx_density'), '75',
    'a timed-out read must leave the DSP alone, not write a default back');

/* ...and the next knob turn must continue from the real value. */
writes.length = 0;
cc(MoveKnob1, 1);
w = writes.find(x => x.key === 'fx_density');
assert.equal(w?.value, '80',
    `after a dead param channel the UI wrote fx_density=${w?.value}; it must ` +
    'continue from 75, not from a zeroed mirror');

console.log('smack chain UI: param-channel and knob-response tests passed');
