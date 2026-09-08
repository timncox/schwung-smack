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

/* ---- knob params -------------------------------------------------------- */

/* Carried over from smack-versio verbatim. See the long comment there: the
 * pot sums with its CV jack in analog hardware, so a knob parked on a step
 * boundary chatters without a deadband, and SEED needs a band of exactly one
 * step because a single step of it re-rolls the entire pattern. */
#define HYST 0.02f

struct Param {
    const char *key;
    const char *label;      /* 4 chars, for the OLED */
    int         lo, hi;
    int         last;       /* last value dispatched; -32768 = never */
    float       last_norm;
};

enum { P_FXD = 0, P_ORD, P_WET, P_SEED, P_LEN, P_RES, P_PITCH, P_COUNT };

static Param P[P_COUNT] = {
    { "fx_density",    "fxd",  0, 100, -32768, 0.0f },
    { "order_density", "ord",  0, 100, -32768, 0.0f },
    { "wet",           "wet",  0, 100, -32768, 0.0f },
    { "seed",          "seed", 0, 127, -32768, 0.0f },
    { "loop_len",      "len",  3,   6, -32768, 0.0f },
    { "slice_res",     "res",  0,   3, -32768, 0.0f },
    { "pitch_range",   "ptch", 1,  24, -32768, 0.0f },
};

/* Page 0 and 1 are knob params; page 2 is the clock, which on the Versio was
 * two hardware switches and here rides on knobs 1 and 2. */
#define PAGE_COUNT 3
static const char *const PAGE_NAME[PAGE_COUNT] = { "FX", "LOOP", "CLOCK" };
static int g_page;

static float deadband_for(const Param &p, int idx)
{
    int span = p.hi - p.lo;
    if(span <= 0)      return 0.0f;
    if(idx == P_SEED)  return 1.0f / (float)span; /* exactly one seed */
    if(span <= 24)     return HYST;
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

/* Pickup. Four knobs address seven params across pages, and the Patch's knobs
 * are absolute -- without this, changing page slams whatever is under the pot
 * into the engine. A knob is inert until it crosses the value it takes over.
 * The Versio never needed this because its knobs were one-to-one with params. */
static bool  g_live[4];
static float g_knob_at[4];
#define PICKUP_SLOP 0.02f

static int page_param(int page, int knob)
{
    int idx = page * 4 + knob;
    return (page < 2 && idx < P_COUNT) ? idx : -1;
}

static void page_reset(void)
{
    for(int k = 0; k < 4; k++)
    {
        g_live[k]    = false;
        g_knob_at[k] = hw.GetKnobValue((DaisyPatch::Ctrl)k);
    }
}

static void dispatch_knobs(void)
{
    char buf[16];
    for(int k = 0; k < 4; k++)
    {
        int idx = page_param(g_page, k);
        if(idx < 0) continue;

        Param &p    = P[idx];
        float  norm = hw.GetKnobValue((DaisyPatch::Ctrl)k);

        if(!g_live[k])
        {
            /* Nothing dispatched yet on this param: wait for movement.
             * Otherwise wait for the knob to cross the live value. */
            if(p.last == -32768)
            {
                if(fabsf(norm - g_knob_at[k]) > PICKUP_SLOP) g_live[k] = true;
            }
            else
            {
                float at = (float)(p.last - p.lo) / (float)(p.hi - p.lo);
                if((g_knob_at[k] <= at && norm >= at)
                   || (g_knob_at[k] >= at && norm <= at))
                    g_live[k] = true;
            }
            g_knob_at[k] = norm;
            if(!g_live[k]) continue;
        }
        g_knob_at[k] = norm;

        /* Deadband, so summed CV noise can't oscillate a knob across a step. */
        float band = deadband_for(p, idx);
        if(p.last != -32768 && fabsf(norm - p.last_norm) < band) continue;

        int v = quantize(p, norm);
        if(v == p.last) continue;

        snprintf(buf, sizeof(buf), "%d", v);
        smack_set_param(S, p.key, buf);
        p.last      = v;
        p.last_norm = norm;
    }
}

/* Page 2: clock ratio and mode, quantised off knobs 1 and 2. These were
 * SW_0 and SW_1 on the Versio panel. */
static int g_ratio_sel = 1, g_mode_sel = 2;

static void dispatch_clock_page(void)
{
    int r = (int)(hw.GetKnobValue(DaisyPatch::CTRL_1) * 2.99f);
    int m = (int)(hw.GetKnobValue(DaisyPatch::CTRL_2) * 2.99f);
    if(r != g_ratio_sel)
    {
        g_ratio_sel = r;
        clk_set_ratio(&CLK, r == 0 ? CLK_TICKS_DIV2
                          : r == 2 ? CLK_TICKS_2X
                                   : CLK_TICKS_1X);
    }
    if(m != g_mode_sel)
    {
        g_mode_sel = m;
        clk_set_mode(&CLK, m == 0 ? CLK_EXTERNAL
                         : m == 1 ? CLK_INFER
                                  : CLK_AUTO);
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
 * that tells the rest of the rack.
 *
 *   CV Out 1   playhead through the captured loop, 0-5 V ramp per pass
 *   CV Out 2   wet amount, 0-5 V
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

    int wet = 0;
    if(smack_get_param(S, "wet", buf, sizeof(buf)) >= 0) wet = atoi(buf);
    if(wet < 0) wet = 0;
    if(wet > 100) wet = 100;
    hw.seed.dac.WriteValue(DacHandle::Channel::TWO,
                           (uint16_t)(wet * 4095 / 100));

    hw.gate_output.Write(System::GetNow() < g_gate_until);
}

/* ---- gestures ----------------------------------------------------------- */

/* Mapping is the one arrived at by playing smack-versio (v0.2.0, after it was
 * REVERSED from the original): tap = re-roll, because re-roll is the gesture
 * used constantly; hold = capture, because capture is deliberate and happens
 * once. Long hold clears, double-tap toggles live. */
#define HOLD_CAPTURE_MS 600u
#define HOLD_CLEAR_MS   2000u
#define DOUBLE_TAP_MS   350u

static bool     enc_down;
static bool     enc_long_done;
static uint32_t enc_last_release;

static void gestures(void)
{
    uint32_t now = System::GetNow();

    if(hw.encoder.RisingEdge())
    {
        enc_down      = true;
        enc_long_done = false;
    }

    if(enc_down && !enc_long_done && hw.encoder.Pressed())
    {
        uint32_t held = hw.encoder.TimeHeldMs();
        if(held > HOLD_CLEAR_MS)
        {
            smack_set_param(S, "clear", "1");
            enc_long_done = true;
        }
        else if(held > HOLD_CAPTURE_MS)
        {
            /* Fires at the threshold, not on release, so the player gets the
             * capture the moment they feel the hold land. */
            smack_set_param(S, "capture", "1");
            enc_long_done = true;
        }
    }

    if(enc_down && !hw.encoder.Pressed())
    {
        enc_down = false;
        if(!enc_long_done)
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

static void draw(void)
{
    char line[32], buf[16];
    hw.display.Fill(false);

    /* Header: page, transport, tempo. All three were invisible on the Versio. */
    const char *st = "--";
    if(smack_get_param(S, "run_state", buf, sizeof(buf)) >= 0) st = buf;
    snprintf(line, sizeof(line), "%-5s %-4s %3d%s",
             PAGE_NAME[g_page], st, (int)(clk_bpm(&CLK) + 0.5f),
             clk_locked(&CLK) ? "" : "?");
    hw.display.SetCursor(0, 0);
    hw.display.WriteString(line, Font_6x8, true);

    if(g_page < 2)
    {
        for(int k = 0; k < 4; k++)
        {
            int idx = page_param(g_page, k);
            if(idx < 0) continue;
            Param &p = P[idx];
            snprintf(line, sizeof(line), "%-4s %-4d%s", p.label,
                     p.last == -32768 ? 0 : p.last, g_live[k] ? "" : " *");
            hw.display.SetCursor(0, 16 + k * 10);
            hw.display.WriteString(line, Font_6x8, true);
        }
    }
    else
    {
        static const char *R[3] = { "/2", "=1", "x2" };
        static const char *M[3] = { "EXT", "INFER", "AUTO" };
        snprintf(line, sizeof(line), "ratio %s", R[g_ratio_sel]);
        hw.display.SetCursor(0, 16);
        hw.display.WriteString(line, Font_6x8, true);
        snprintf(line, sizeof(line), "mode  %s", M[g_mode_sel]);
        hw.display.SetCursor(0, 26);
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
    clk_set_ratio(&CLK, CLK_TICKS_1X);
    clk_set_mode(&CLK, CLK_AUTO);

    hw.StartAdc();
    hw.StartAudio(AudioCallback);
    hw.midi.StartReceive();

    page_reset();
    uint32_t last_draw = System::GetNow();

    for(;;)
    {
        hw.ProcessAllControls();

        int inc = hw.encoder.Increment();
        if(inc)
        {
            g_page += inc;
            while(g_page < 0)           g_page += PAGE_COUNT;
            while(g_page >= PAGE_COUNT) g_page -= PAGE_COUNT;
            page_reset();
        }

        gestures();

        if(g_page < 2) dispatch_knobs();
        else           dispatch_clock_page();

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

        uint32_t now = System::GetNow();
        if(now - last_draw >= 50u)
        {
            draw();
            last_draw = now;
        }
    }
}
