/*
 * Smack — Electrosmith Daisy Patch port.
 *
 * Smack is quantized live-loop capture with seeded per-slice glitch FX. The
 * engine (vendor/smack_core.c) is byte-identical to schwung-smack; only two
 * constants differ, both documented in vendor/smack_core.h. This file is the
 * Daisy shim, the counterpart of src/smack_fx.c on Move.
 *
 * Relationship to smack-versio: that port targets the Noise Engineering
 * Versio, which has 7 knobs, 2 three-position switches, one button, one gate
 * and 4 LEDs. The Patch trades most of that for an OLED and an encoder. What
 * carries over verbatim, because it was learned by playing the module rather
 * than reasoned out, is the knob deadband/hysteresis logic below -- keep it.
 *
 * What the screen changes: on the Versio, the run state and CPU load had to
 * be replayed on LEDs at the *next* power-up, because "LED 3's live alarm
 * can't be read by someone playing with both hands." Here they are simply on
 * screen while you play.
 */
#include "daisy_patch.h"
#include "patch_alloc.h"
#include "module_picker.h"

extern "C" {
#include "vendor/smack_core.h"
#include "vendor/plugin_api_v1.h"
#include "clock_adapter.h"
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace daisy;

static DaisyPatch      hw;
static smack_t        *S;
static clock_adapter_t CLK;
static host_api_v1_t   HOST;

/* 128 is not a preference. The engine's clock regression was built for
 * 128-frame callbacks; at Daisy's default 48 the retro-capture phase
 * alignment fails. Reproduced at 44.1 kHz too, so it is block size, not
 * rate. Do not change this without re-running that test. */
#define BLOCK_SIZE 128

/* SMACK_RING_FRAMES is SMACK_SR * 70 = 3,360,000 frames; stereo int16 makes
 * the ring 13.44 MB. 16 MB leaves room for the per-lane delay and reverb
 * lines smack_create() also takes. Same pool size smack-versio uses. */
#define POOL_BYTES (16u * 1024u * 1024u)
static uint8_t DSY_SDRAM_BSS g_pool[POOL_BYTES];

/* ---- control surface ---------------------------------------------------- */

/*
 * The Patch has four knobs and four CV inputs, and libDaisy exposes them as
 * FOUR analog controls, not eight (DaisyPatch::CTRL_LAST == 4): each CV jack
 * is summed with its knob before the ADC, so a CV input can only ever be read
 * together with its knob. Two things follow, and this file is built on both:
 *
 *   1. A param that takes CV has to own its knob permanently. The first cut
 *      paged four knobs across seven params with pickup; the moment a cable
 *      went into CV 1, that voltage would have steered whichever param was on
 *      the current page, and pickup would have "picked up" on the CV's own
 *      motion. So the four CV'd params are fixed to the four knobs, there are
 *      no pages, and there is no pickup -- the knobs are one-to-one with
 *      their params exactly as on the Versio.
 *
 *   2. Everything else lives on the encoder: turn to select, push-and-turn
 *      to edit. That is what the screen is for.
 *
 *   Knob 1 + CV 1   fx_density      how many slices get an effect
 *   Knob 2 + CV 2   order_density   how scrambled the slice order is
 *   Knob 3 + CV 3   wet             clean loop <-> glitched pattern
 *   Knob 4 + CV 4   slice_res       how finely the loop is cut
 *
 * FX and ORDER are the two the Versio manual singles out ("FX and ORDER under
 * CV is the thing this version can do that the Move version cannot"). Note
 * what a CV sweep of them actually does: every change of one unit re-rolls
 * the pattern (smack_set_param calls roll_pattern), deterministically for
 * the current seed. A slow CV walks through related patterns; a fast LFO is
 * a re-roll machine. Both are legitimate; know which one you patched.
 */

/* Carried over from smack-versio verbatim. See the long comment there: the
 * pot sums with its CV jack in analog hardware, so a knob parked on a step
 * boundary chatters without a deadband. (The Versio's SEED special case --
 * a band of exactly one seed -- is gone because SEED is no longer a knob; on
 * the encoder it moves by exact detents and needs no band.) */
#define HYST 0.02f

struct Param {
    const char *key;
    const char *label;      /* 3 chars, for the OLED */
    int         lo, hi;
    int         last;       /* last value dispatched; -32768 = never */
    float       last_norm;
};

enum { P_FXD = 0, P_ORD, P_WET, P_RES, P_COUNT };   /* == knob order */

static Param P[P_COUNT] = {
    { "fx_density",    "fxd", 0, 100, -32768, 0.0f },
    { "order_density", "ord", 0, 100, -32768, 0.0f },
    { "wet",           "wet", 0, 100, -32768, 0.0f },
    { "slice_res",     "res", 0,   3, -32768, 0.0f },
};

static float deadband_for(const Param &p)
{
    int span = p.hi - p.lo;
    if(span <= 0)  return 0.0f;
    if(span <= 24) return HYST;  /* stepped: slice_res */
    return 0.0f; /* genuinely continuous: one unit of change is inaudible */
}

static int quantize(const Param &p, float norm)
{
    int span = p.hi - p.lo;
    int v    = p.lo + (int)(norm * (float)span + 0.5f);
    if(v < p.lo) v = p.lo;
    if(v > p.hi) v = p.hi;
    return v;
}

/* smack-versio's dispatch_knobs, with the Patch's control API. Absolute knobs,
 * no pickup: what the knob (plus its CV) says is what the engine gets. */
static void dispatch_knobs(void)
{
    char buf[16];
    for(int k = 0; k < P_COUNT; k++)
    {
        Param &p    = P[k];
        float  norm = hw.GetKnobValue((DaisyPatch::Ctrl)k);
        if(norm < 0.0f) norm = 0.0f;
        if(norm > 1.0f) norm = 1.0f;

        int v = quantize(p, norm);
        if(v == p.last) continue;

        /* Deadband, so summed CV noise can't oscillate a knob across a step
         * boundary. */
        float dead = deadband_for(p);
        if(dead > 0.0f && p.last != -32768)
        {
            float moved = norm - p.last_norm;
            if(moved < 0.0f) moved = -moved;
            if(moved < dead) continue;
        }

        p.last      = v;
        p.last_norm = norm;
        snprintf(buf, sizeof(buf), "%d", v);
        smack_set_param(S, p.key, buf);
    }
}

/* Knobs are dispatched at about block rate, as the Versio does (it calls
 * dispatch_knobs from the audio callback, ~375 Hz at 48k/128). fx_density
 * and order_density re-roll the pattern on every change, so this is also
 * the ceiling on how often a CV input can. */
#define KNOB_MS 3u

/* ---- encoder menu ------------------------------------------------------- */

/*
 * The params that do not take CV. Turn selects, push-and-turn edits.
 *
 * The three engine-side values are read back from the engine for display
 * rather than mirrored here, so a re-roll, a MIDI CC or a state restore that
 * changes them shows up without any bookkeeping. Ratio and mode belong to the
 * clock adapter (they were the Versio's two switches) and live in the shim.
 */
enum { M_SEED = 0, M_LEN, M_PITCH, M_RATIO, M_MODE, M_MODS, M_COUNT };
static const char *const MENU_LABEL[M_COUNT] = { "seed", "len", "ptch", "rat", "clk", "mods" };
static int g_menu_sel;

static int g_ratio_sel = 1, g_mode_sel = 2;
static const char *const RATIO_NAME[3] = { "/2", "=1", "x2" };
static const char *const MODE_NAME[3]  = { "EXT", "INF", "AUTO" };

/* loop_len is an index into the engine's half-step table: 0..8 = 1 step ..
 * 16 bars, i.e. (1 << idx) steps. The Versio's LENGTH knob offered 3..6
 * (8/16/32/64 steps); on an encoder there is no reason to hide the rest. */
#define LEN_MAX 8
#define SEED_MAX 9999   /* the Move UI's seed range */

static int engine_int(const char *key)
{
    char buf[16];
    if(smack_get_param(S, key, buf, sizeof(buf)) < 0) return 0;
    return atoi(buf);
}

static void set_engine_int(const char *key, int v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", v);
    smack_set_param(S, key, buf);
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void apply_clock(void)
{
    clk_set_ratio(&CLK, g_ratio_sel == 0 ? CLK_TICKS_DIV2
                      : g_ratio_sel == 2 ? CLK_TICKS_2X
                                         : CLK_TICKS_1X);
    clk_set_mode(&CLK, g_mode_sel == 0 ? CLK_EXTERNAL
                     : g_mode_sel == 1 ? CLK_INFER
                                       : CLK_AUTO);
}

static void menu_edit(int inc)
{
    switch(g_menu_sel)
    {
        case M_SEED:
            /* Dialing the seed browses patterns directly (engine comment on
             * "seed"); it also resets the re-roll nonce, so the pattern shown
             * is the canonical one for that number. */
            set_engine_int("seed", clampi(engine_int("seed") + inc, 0, SEED_MAX));
            break;
        case M_LEN:
            set_engine_int("loop_len", clampi(engine_int("loop_len") + inc, 0, LEN_MAX));
            break;
        case M_PITCH:
            set_engine_int("pitch_range", clampi(engine_int("pitch_range") + inc, 1, 24));
            break;
        case M_RATIO:
            g_ratio_sel = clampi(g_ratio_sel + inc, 0, 2);
            apply_clock();
            break;
        case M_MODE:
            g_mode_sel = clampi(g_mode_sel + inc, 0, 2);
            apply_clock();
            break;
        case M_MODS:
            /* Modal: returns when the user backs out, never if a module was
             * loaded. encoder() marks this press as spent on return. */
            picker::run(hw);
            break;
    }
}

static void menu_value(int item, char *out, size_t n)
{
    switch(item)
    {
        case M_SEED:  snprintf(out, n, "%d", engine_int("seed")); break;
        case M_LEN:   snprintf(out, n, "%d", 1 << clampi(engine_int("loop_len"), 0, LEN_MAX)); break;
        case M_PITCH: snprintf(out, n, "%d", engine_int("pitch_range")); break;
        case M_RATIO: snprintf(out, n, "%s", RATIO_NAME[g_ratio_sel]); break;
        case M_MODE:  snprintf(out, n, "%s", MODE_NAME[g_mode_sel]); break;
        case M_MODS:  snprintf(out, n, "%s", "..."); break;
        default:      out[0] = '\0'; break;
    }
}

/* ---- audio -------------------------------------------------------------- */

static int16_t bufi[BLOCK_SIZE * 2];

static void emit_to_engine(void *ctx, uint8_t byte)
{
    smack_on_midi((smack_t *)ctx, &byte, 1, 3); /* source 3 = host, as on Move */
}

/* DaisyPatch::StartAudio takes the NON-interleaving callback (the Versio's is
 * interleaving). Audio In 1/2 are the stereo source, Out 1/2 the result.
 *
 * Outs 3/4 carry the DRY input as a straight thru. Smack is a glitcher, so
 * having the untouched signal on its own pair is what lets you crossfade or
 * gate between clean and mangled downstream instead of committing to `wet`. */
static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    const size_t n = size < BLOCK_SIZE ? size : BLOCK_SIZE;

    for(size_t i = 0; i < n; i++)
    {
        float l = in[0][i], r = in[1][i];
        if(l > 1.0f) l = 1.0f; else if(l < -1.0f) l = -1.0f;
        if(r > 1.0f) r = 1.0f; else if(r < -1.0f) r = -1.0f;
        bufi[i * 2]     = (int16_t)(l * 32767.0f);
        bufi[i * 2 + 1] = (int16_t)(r * 32767.0f);
    }

    smack_process(S, bufi, bufi, (int)n);

    for(size_t i = 0; i < n; i++)
    {
        out[0][i] = (float)bufi[i * 2]     / 32767.0f;
        out[1][i] = (float)bufi[i * 2 + 1] / 32767.0f;
        out[2][i] = in[0][i];    /* dry thru */
        out[3][i] = in[1][i];
    }

    clk_advance(&CLK, (int)n, emit_to_engine, S);
}

/* ---- CV and gate outputs ------------------------------------------------ */

/*
 * The Versio has four LEDs; the Patch has two CV outputs and a gate. That is
 * the difference between a module that shows you what it is doing and one
 * that tells the rest of the rack. The rule for what goes on them: something
 * only Smack knows and that moves on its own. A knob position is neither --
 * the rack already has the knob -- so nothing here echoes one.
 *
 *   CV Out 1   playhead through the captured loop, 0-5 V ramp per pass
 *   CV Out 2   the source slice under the playhead, stepped 0-5 V: a
 *              staircase on the clean side, and on the pattern side it
 *              jumps exactly the way the reorder does. A sequencer that
 *              plays Smack's shuffle; re-roll gives it a new sequence.
 *   Gate Out   pulse on every loop wrap
 *
 * The gate is the useful one: Smack's loop length is whatever the player
 * captured, so this is a clock at the musical period actually in the buffer.
 * Nothing derived from a master clock knows that length.
 *
 * play_frame is formatted "%.0f" by the engine, which is precisely why the
 * Makefile links -u _printf_float. Without it this reads "" and the ramp sits
 * at zero forever -- the same silent failure that broke smack-versio's LIVE
 * mode. If the ramp is dead on hardware, check the ELF for _printf_float
 * before suspecting anything here.
 */
#define GATE_MS 5u

static uint32_t g_gate_until;
static long     g_pf_prev;

static void update_cv_outs(void)
{
    char buf[24];
    long pf = -1, lf = 0;

    if(smack_get_param(S, "play_frame", buf, sizeof(buf)) >= 0)
        pf = atol(buf);
    if(smack_get_param(S, "loop_frames", buf, sizeof(buf)) >= 0)
        lf = atol(buf);

    if(pf >= 0 && lf > 0)
    {
        if(pf > lf) pf = lf;
        hw.seed.dac.WriteValue(DacHandle::Channel::ONE,
                               (uint16_t)((uint64_t)pf * 4095u / (uint64_t)lf));
        if(pf < g_pf_prev)          /* wrapped: top of the loop */
            g_gate_until = System::GetNow() + GATE_MS;
        g_pf_prev = pf;
    }

    /* play_source is -1 while idle, which parks the output at 0 V. */
    int src = engine_int("play_source");
    int n   = engine_int("n_slices");
    uint16_t v2 = 0;
    if(src >= 0 && n > 1)
    {
        if(src >= n) src = n - 1;
        v2 = (uint16_t)((uint32_t)src * 4095u / (uint32_t)(n - 1));
    }
    hw.seed.dac.WriteValue(DacHandle::Channel::TWO, v2);

    hw.gate_output.Write(System::GetNow() < g_gate_until);
}

/* ---- encoder: menu + gestures ------------------------------------------- */

/* Press gestures are the ones arrived at by playing smack-versio (v0.2.0,
 * after they were REVERSED from the original): tap = re-roll, because re-roll
 * is the gesture used constantly; hold = capture, because capture is
 * deliberate and happens once. Long hold clears, double-tap toggles live.
 *
 * Turning is new here. Encoder up: the turn moves the menu cursor. Encoder
 * held: the turn edits the selected item, and that press is then spent --
 * releasing it will not re-roll and holding on will not capture. Press, then
 * turn promptly: a hold that has already passed HOLD_CAPTURE_MS has already
 * captured by the time you turn. */
#define HOLD_CAPTURE_MS 600u
#define HOLD_CLEAR_MS   2000u
#define DOUBLE_TAP_MS   350u

static bool     enc_down;
static bool     enc_turned;     /* this press edited the menu */
static bool     enc_captured;   /* this press has fired capture */
static bool     enc_cleared;    /* this press has fired clear */
static uint32_t enc_last_release;

static void encoder(void)
{
    uint32_t now = System::GetNow();

    if(hw.encoder.RisingEdge())
    {
        enc_down     = true;
        enc_turned   = false;
        enc_captured = false;
        enc_cleared  = false;
    }

    int inc = hw.encoder.Increment();
    if(inc)
    {
        if(enc_down && hw.encoder.Pressed())
        {
            menu_edit(inc);
            enc_turned = true;
        }
        else
        {
            g_menu_sel += inc;
            while(g_menu_sel < 0)        g_menu_sel += M_COUNT;
            while(g_menu_sel >= M_COUNT) g_menu_sel -= M_COUNT;
        }
    }

    if(enc_down && !enc_turned && hw.encoder.Pressed())
    {
        uint32_t held = (uint32_t)hw.encoder.TimeHeldMs();
        /* Capture fires at the threshold, not on release, so the player gets
         * it the moment they feel the hold land. Keep holding to HOLD_CLEAR_MS
         * and the loop is cleared -- the capture just taken is discarded with
         * it, which is the intent of a hold that long. (The first cut's clear
         * could never fire: one flag gated both thresholds.) */
        if(!enc_captured && held > HOLD_CAPTURE_MS)
        {
            smack_set_param(S, "capture", "1");
            enc_captured = true;
        }
        if(!enc_cleared && held > HOLD_CLEAR_MS)
        {
            smack_set_param(S, "clear", "1");
            enc_cleared = true;
        }
    }

    if(enc_down && !hw.encoder.Pressed())
    {
        enc_down = false;
        if(!enc_turned && !enc_captured)
        {
            if(now - enc_last_release < DOUBLE_TAP_MS)
                smack_set_param(S, "live", "1");
            else
                smack_set_param(S, "reroll", "1");
            enc_last_release = now;
        }
    }
}

/* ---- display ------------------------------------------------------------ */

/*
 * 128x64, Font_6x8: 21 columns, rows at y = 0, 16, 26, 36, 46, 56.
 *
 *   LOOP 120  B          <- state, tempo ('?' until the clock locks), side
 *   fxd 100  >seed 4303  <- knobs on the left, live
 *   ord  35   len    16     menu on the right, '>' is the cursor
 *   wet 100   ptch   12
 *   res   2   rat    =1
 *             clk   AUTO
 */
static const char *const STATE_NAME[4] = { "IDLE", "ARM", "REC", "LOOP" };
#define MENU_X 66

static void draw(void)
{
    char line[32], val[12];
    hw.display.Fill(false);

    int st = engine_int("run_state");
    snprintf(line, sizeof(line), "%-4s %3d%s  %s",
             (st >= 0 && st < 4) ? STATE_NAME[st] : "?",
             (int)(clk_bpm(&CLK) + 0.5f),
             clk_locked(&CLK) ? "" : "?",
             engine_int("ab") ? "B" : "A");
    hw.display.SetCursor(0, 0);
    hw.display.WriteString(line, Font_6x8, true);

    for(int k = 0; k < P_COUNT; k++)
    {
        snprintf(line, sizeof(line), "%-3s %3d", P[k].label,
                 P[k].last == -32768 ? 0 : P[k].last);
        hw.display.SetCursor(0, 16 + k * 10);
        hw.display.WriteString(line, Font_6x8, true);
    }

    for(int i = 0; i < M_COUNT; i++)
    {
        menu_value(i, val, sizeof(val));
        snprintf(line, sizeof(line), "%c%-4s %4s",
                 i == g_menu_sel ? '>' : ' ', MENU_LABEL[i], val);
        hw.display.SetCursor(MENU_X, 16 + i * 10);
        hw.display.WriteString(line, Font_6x8, true);
    }

    hw.display.Update();
}

/* ---- main --------------------------------------------------------------- */

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(BLOCK_SIZE);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);

    patch_alloc_init(g_pool, POOL_BYTES);

    HOST.api_version      = 1;
    HOST.sample_rate      = SMACK_SR;
    HOST.frames_per_block = BLOCK_SIZE;

    S = smack_create(&HOST);

    if(!S || patch_alloc_failed())
    {
        hw.display.Fill(false);
        hw.display.SetCursor(0, 0);
        hw.display.WriteString("SMACK: alloc fail", Font_6x8, true);
        hw.display.SetCursor(0, 16);
        char l[32];
        snprintf(l, sizeof(l), "%u/%u KB",
                 (unsigned)(patch_alloc_used() / 1024u),
                 (unsigned)(patch_alloc_capacity() / 1024u));
        hw.display.WriteString(l, Font_6x8, true);
        hw.display.Update();
        for(;;) {}
    }

    clk_init(&CLK, SMACK_SR, 120.0f);
    apply_clock();

    hw.StartAdc();
    hw.StartAudio(AudioCallback);
    hw.midi.StartReceive();

    uint32_t last_draw  = System::GetNow();
    uint32_t last_knobs = last_draw;

    for(;;)
    {
        hw.ProcessAllControls();

        encoder();

        uint32_t now = System::GetNow();
        if(now - last_knobs >= KNOB_MS)
        {
            dispatch_knobs();
            last_knobs = now;
        }

        /* Gate 1 is the clock/trigger jack, as on the Versio. Gate 2 has no
         * Versio equivalent -- the Patch simply has a second one -- so it is
         * a direct capture trigger, the gesture you most want footswitchable. */
        if(hw.gate_input[0].Trig()) clk_gate_edge(&CLK);
        if(hw.gate_input[1].Trig()) smack_set_param(S, "capture", "1");

        hw.midi.Listen();
        while(hw.midi.HasEvents())
        {
            MidiEvent ev = hw.midi.PopEvent();
            if(ev.type == ControlChange)
            {
                uint8_t msg[3] = { (uint8_t)(0xB0 | (ev.channel & 0x0F)),
                                   (uint8_t)ev.data[0],
                                   (uint8_t)ev.data[1] };
                smack_on_midi(S, msg, 3, 2); /* 2 = external */
            }
        }

        /* CV and gate follow the playhead, so they run every loop pass. A
         * loop-wrap gate that lagged 50 ms would be useless as a clock. */
        update_cv_outs();

        if(now - last_draw >= 50u)
        {
            draw();
            last_draw = now;
        }
    }
}
