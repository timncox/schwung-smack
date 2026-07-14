/*
 * Native host simulation for the Smack core — no hardware needed.
 *
 * Feeds a 220 Hz saw through the engine block-by-block while sending MIDI
 * clock at 120 BPM, then exercises: retro capture, A/B, reroll, order
 * density, arm/record. Asserts state transitions and basic output sanity.
 *
 * Build & run:  make test
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/smack_core.h"

#define BLK 128
#define FPT 918.75 /* frames per MIDI clock tick at 120 BPM */

static float fake_bpm(void) { return 120.0f; }

static smack_t *S;
static double next_tick = 0.0;
static uint64_t frames_done = 0;

/* l_on/r_on: which input channels carry the test saw (dual-mono tests
 * feed one side only) */
static void run_blocks_lr(int nblocks, int16_t *last_out, int l_on, int r_on) {
    static double phase = 0.0;
    int16_t in[BLK * 2], out[BLK * 2];
    for (int b = 0; b < nblocks; b++) {
        /* clock ticks due before this block */
        while (next_tick <= (double)frames_done) {
            uint8_t tick = 0xF8;
            smack_on_midi(S, &tick, 1, 3);
            next_tick += FPT;
        }
        for (int i = 0; i < BLK; i++) {
            phase += 220.0 / 44100.0;
            if (phase >= 1.0) phase -= 1.0;
            int16_t v = (int16_t)((phase * 2.0 - 1.0) * 12000.0);
            in[i * 2]     = l_on ? v : 0;
            in[i * 2 + 1] = r_on ? v : 0;
        }
        smack_process(S, in, out, BLK);
        frames_done += BLK;
    }
    if (last_out) memcpy(last_out, out, sizeof(out));
}

static void run_blocks(int nblocks, int16_t *last_out) {
    run_blocks_lr(nblocks, last_out, 1, 1);
}

static void run_constant_blocks(int nblocks, int16_t value, int16_t *last_out) {
    int16_t in[BLK * 2], out[BLK * 2];
    for (int i = 0; i < BLK * 2; i++) in[i] = value;
    for (int b = 0; b < nblocks; b++) {
        smack_process(S, in, out, BLK);
        frames_done += BLK;
    }
    if (last_out) memcpy(last_out, out, sizeof(out));
}

/* Beat-gated saw: 25%-duty bursts at the given BPM (for BPM detection) */
static void run_blocks_beat(int nblocks, double bpm) {
    static double phase = 0.0;
    const double period = 60.0 / bpm * 44100.0;
    int16_t in[BLK * 2], out[BLK * 2];
    for (int b = 0; b < nblocks; b++) {
        while (next_tick <= (double)frames_done) {
            uint8_t tick = 0xF8;
            smack_on_midi(S, &tick, 1, 3);
            next_tick += FPT;
        }
        for (int i = 0; i < BLK; i++) {
            phase += 220.0 / 44100.0;
            if (phase >= 1.0) phase -= 1.0;
            double beat_pos = fmod((double)frames_done + i, period);
            int on = beat_pos < period * 0.25;
            int16_t v = on ? (int16_t)((phase * 2.0 - 1.0) * 12000.0) : 0;
            in[i * 2] = v;
            in[i * 2 + 1] = v;
        }
        smack_process(S, in, out, BLK);
        frames_done += BLK;
    }
}

static long energy(const int16_t *buf) {
    long e = 0;
    for (int i = 0; i < BLK * 2; i++) e += labs((long)buf[i]);
    return e;
}

static char gp(const char *key) {
    char buf[64];
    assert(smack_get_param(S, key, buf, sizeof(buf)) >= 0);
    return buf[0];
}

static void reset_sim_instance(const host_api_v1_t *host) {
    S = smack_create(host);
    assert(S);
    next_tick = 0.0;
    frames_done = 0;
}

static void test_fixed_record_quantum(const host_api_v1_t *host) {
    reset_sim_instance(host);
    smack_set_param(S, "loop_len", "0");       /* one 16th at 120 = 5512 */
    smack_set_param(S, "bpm_override", "120");
    smack_set_param(S, "arm", "1");
    run_constant_blocks(1, 1000, NULL);
    assert(gp("run_state") == '2');

    /* Parameter edits during the take affect the next gesture, not the
     * length already armed. */
    smack_set_param(S, "bpm_override", "60");
    smack_set_param(S, "loop_len", "1");
    run_constant_blocks(100, 1000, NULL);
    assert(gp("run_state") == '3');
    char buf[32];
    smack_get_param(S, "loop_frames", buf, sizeof(buf));
    assert(atoi(buf) == 5512);

    smack_set_param(S, "fx_density", "999");
    smack_get_param(S, "fx_density", buf, sizeof(buf));
    assert(atoi(buf) == 100);
    smack_set_param(S, "order_density", "-20");
    smack_get_param(S, "order_density", buf, sizeof(buf));
    assert(atoi(buf) == 0);
    smack_set_param(S, "wet", "999");
    smack_get_param(S, "wet", buf, sizeof(buf));
    assert(atoi(buf) == 100);

    smack_destroy(S);
    printf("ok: fixed arm quantum + direct param clamps\n");
}

static void test_stopped_clock_capture_phase(const host_api_v1_t *host) {
    reset_sim_instance(host);
    uint8_t start = 0xFA, stop = 0xFC;
    smack_on_midi(S, &start, 1, 3);
    run_blocks(120, NULL);                    /* establish clock tempo + phase */
    smack_on_midi(S, &stop, 1, 3);
    run_constant_blocks(400, 12345, NULL);    /* several steps after MIDI Stop */

    smack_set_param(S, "loop_len", "0");
    smack_set_param(S, "hw_input", "1");
    smack_set_param(S, "monitor", "0");
    smack_set_param(S, "wet", "0");
    smack_set_param(S, "ab", "0");
    smack_set_param(S, "quantize", "0");
    smack_set_param(S, "capture", "1");
    int16_t out[BLK * 2];
    run_constant_blocks(1, 0, out);
    int recent = 0;
    for (int i = SMACK_EDGE_FADE; i < BLK; i++)
        if (abs((int)out[i * 2] - 12345) <= 1) recent++;
    assert(recent > 25);                     /* capture ended near now */

    smack_destroy(S);
    printf("ok: stopped-clock retro phase extrapolation\n");
}

static void test_paused_loop_ring_guard(const host_api_v1_t *host) {
    reset_sim_instance(host);
    smack_set_param(S, "loop_len", "8");       /* 16 bars */
    smack_set_param(S, "bpm_override", "20"); /* quantum caps at ring size */
    smack_set_param(S, "hw_input", "1");
    smack_set_param(S, "monitor", "0");
    smack_set_param(S, "wet", "0");
    smack_set_param(S, "ab", "0");
    smack_set_param(S, "quantize", "0");
    run_constant_blocks(25000, 10000, NULL);   /* fill the 70-second ring */
    smack_set_param(S, "capture", "1");

    int16_t before[BLK * 2], after[BLK * 2];
    run_constant_blocks(1, 0, before);
    assert(energy(before) > 0);
    uint8_t stop = 0xFC, start = 0xFA;
    smack_on_midi(S, &stop, 1, 3);             /* retained loop, paused */
    run_constant_blocks(1, 0, NULL);           /* must not overwrite its top */
    smack_on_midi(S, &start, 1, 3);            /* resumes from loop top */
    run_constant_blocks(1, 0, after);
    assert(memcmp(before, after, sizeof(before)) == 0);

    smack_destroy(S);
    printf("ok: paused-loop ring overwrite guard\n");
}

int main(void) {
    host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.api_version = 1;
    host.sample_rate = SMACK_SR;
    host.frames_per_block = BLK;
    host.get_bpm = fake_bpm;

    test_fixed_record_quantum(&host);
    test_stopped_clock_capture_phase(&host);
    test_paused_loop_ring_guard(&host);

    reset_sim_instance(&host);

    int16_t out[BLK * 2];

    /* transport start, then 2 bars of audio (1 bar = 88200 frames = ~689 blocks) */
    uint8_t start = 0xFA;
    smack_on_midi(S, &start, 1, 3);
    run_blocks(1400, out);
    assert(gp("run_state") == '0');            /* IDLE, passing through */
    assert(energy(out) > 0);

    /* retro-grab 1 bar (loop_len default idx 4), slice res 1 step */
    smack_set_param(S, "capture", "1");
    assert(gp("run_state") == '3');            /* LOOPING */
    char buf[64];
    smack_get_param(S, "n_slices", buf, sizeof(buf));
    assert(atoi(buf) == 16);               /* 1 bar / 1 step = 16 slices */

    /* B pattern (default) produces audio */
    run_blocks(700, out);
    assert(energy(out) > 0);

    /* switch to A clean at loop quantize, keeps producing audio */
    smack_set_param(S, "quantize", "0");
    smack_set_param(S, "ab", "0");
    run_blocks(700, out);
    assert(energy(out) > 0);

    /* pattern string reflects fx assignments; reroll changes seed/pattern */
    smack_set_param(S, "ab", "1");
    smack_set_param(S, "fx_density", "100");
    smack_get_param(S, "pattern", buf, sizeof(buf));
    char before[64];
    strcpy(before, buf);
    smack_set_param(S, "reroll", "1");
    smack_get_param(S, "pattern", buf, sizeof(buf));
    assert(strlen(buf) == 16);
    int all_none = 1;
    for (int i = 0; buf[i]; i++) if (buf[i] != '0') all_none = 0;
    assert(!all_none);                     /* density 100% => fx everywhere */
    (void)before;

    /* order density shuffles without crashing */
    smack_set_param(S, "order_density", "100");
    run_blocks(700, out);
    assert(energy(out) > 0);

    /* every effect renders without crashing: lock each fx onto slice 0 */
    for (int f = 1; f < SMACK_FX_COUNT; f++) {
        char v[8];
        snprintf(v, sizeof(v), "%d", f);
        smack_set_param(S, "lock_slice_0", v);
        run_blocks(700, out);
        assert(gp("run_state") == '3');
    }
    smack_set_param(S, "lock_slice_0", "-1"); /* unlock */

    /* pinning survives a re-roll; unlock + seed re-dial restores the
     * canonical pattern */
    char patseed[64];
    smack_set_param(S, "seed", "777");
    smack_get_param(S, "pattern", patseed, sizeof(patseed));
    smack_set_param(S, "lock_slice_0", "5");   /* pin gate-chop */
    smack_set_param(S, "reroll", "trigger");
    smack_get_param(S, "pattern", buf, sizeof(buf));
    assert(buf[0] == '0' + 5);                 /* pin held through re-roll */
    smack_set_param(S, "lock_slice_0", "-1");
    smack_set_param(S, "seed", "777");
    smack_get_param(S, "pattern", buf, sizeof(buf));
    assert(strcmp(buf, patseed) == 0);         /* seeded pattern restored */

    /* transport follow: Stop pauses the loop, Start resumes from the top */
    uint8_t stop = 0xFC;
    smack_on_midi(S, &stop, 1, 3);
    run_blocks(5, out);
    assert(gp("run_state") == '3');            /* loop retained while paused */
    smack_on_midi(S, &start, 1, 3);
    run_blocks(700, out);
    assert(energy(out) > 0);               /* resumed */

    /* seed: readable, settable, and deterministic (same seed = same pattern) */
    char p1[64], p2[64];
    assert(smack_get_param(S, "seed", buf, sizeof(buf)) > 0);
    smack_set_param(S, "seed", "4242");
    smack_get_param(S, "seed", buf, sizeof(buf));
    assert(atoi(buf) == 4242);
    smack_get_param(S, "pattern", p1, sizeof(p1));
    smack_set_param(S, "reroll", "trigger"); /* host trigger-enum string */
    smack_get_param(S, "seed", buf, sizeof(buf));
    assert(atoi(buf) == 4242);             /* reroll never mutates the seed */
    smack_set_param(S, "seed", "4242");    /* re-dial resets the nonce */
    smack_get_param(S, "pattern", p2, sizeof(p2));
    assert(strcmp(p1, p2) == 0);           /* seed 4242 reproduces exactly */

    /* state round-trip: snapshot, mutate, restore, verify */
    char snap[1024], check[64];
    assert(smack_get_param(S, "state", snap, sizeof(snap)) > 0);
    smack_set_param(S, "seed", "1111");
    smack_set_param(S, "fx_density", "10");
    smack_set_param(S, "state", snap);
    smack_get_param(S, "fx_density", check, sizeof(check));
    assert(atoi(check) == 100);            /* restored */

    /* pinned lock round-trips with its parameter (i:f:p triplet).
     * Pin twice so the final pin is a change and gets the canonical fxp. */
    smack_set_param(S, "lock_slice_1", "4");            /* pin speed first */
    smack_set_param(S, "lock_slice_1", "3");            /* then varispeed */
    assert(smack_get_param(S, "state", snap, sizeof(snap)) > 0);
    assert(strstr(snap, "\"locks\":\"1:3:5\""));        /* canonical fxp 5 */
    smack_set_param(S, "lock_slice_1", "-1");           /* unlock */
    smack_set_param(S, "state", snap);                  /* restore */
    smack_get_param(S, "pattern", buf, sizeof(buf));
    assert(buf[1] == '0' + 3);                          /* pin restored */
    smack_set_param(S, "lock_slice_1", "-1");

    /* clear -> arm -> records exactly one loop then loops again */
    smack_set_param(S, "clear", "1");
    assert(gp("run_state") == '0');
    smack_set_param(S, "arm", "1");
    assert(gp("run_state") == '1');            /* ARMED */
    smack_set_param(S, "arm", "1");        /* repeat fire: harmless while ARMED */
    smack_set_param(S, "arm", "0");        /* zero is always a no-op */
    run_blocks(60, NULL);                  /* > 6 ticks: spans a step boundary */
    assert(gp("run_state") == '2' || gp("run_state") == '3');
    run_blocks(1400, out);                 /* > 1 bar: recording must finish */
    assert(gp("run_state") == '3');
    assert(energy(out) > 0);
    smack_set_param(S, "clear", "0");      /* zero must NOT clear */
    assert(gp("run_state") == '3');
    smack_set_param(S, "clear", "trigger"); /* host trigger string fires */
    assert(gp("run_state") == '0');

    /* ---- dual mono: L/R as independent lanes, panned into the field ---- */
    smack_set_param(S, "channel_mode", "1");
    smack_set_param(S, "fx_density", "100");
    smack_set_param(S, "order_density", "50");
    smack_set_param(S, "wet", "100");
    smack_set_param(S, "ab", "1");
    smack_set_param(S, "quantize", "0");
    run_blocks_lr(1400, out, 1, 0);        /* left-only input */
    smack_set_param(S, "capture", "1");
    run_blocks_lr(200, out, 1, 0);
    assert(gp("run_state") == '3');

    char pl[64], pr[64];
    smack_get_param(S, "pattern", pl, sizeof(pl));
    smack_get_param(S, "pattern_r", pr, sizeof(pr));
    assert(strlen(pl) == 16 && strlen(pr) == 16);
    assert(strcmp(pl, pr) != 0);           /* lanes draw independently */

    /* lane-R lock is independent of lane L */
    smack_set_param(S, "lock_slice_r_0", "5");
    smack_get_param(S, "pattern_r", pr, sizeof(pr));
    assert(pr[0] == '0' + 5);
    smack_get_param(S, "pattern", pl, sizeof(pl));

    /* left-only input, pan_l default 0: energy stays on the left output */
    long el = 0, er = 0;
    run_blocks_lr(700, out, 1, 0);
    for (int i = 0; i < BLK; i++) { el += labs((long)out[i * 2]); er += labs((long)out[i * 2 + 1]); }
    assert(el > 0);
    assert(er < el / 8);

    /* swing lane L hard right: energy moves to the right output */
    smack_set_param(S, "pan_l", "100");
    run_blocks_lr(700, out, 1, 0);
    el = er = 0;
    for (int i = 0; i < BLK; i++) { el += labs((long)out[i * 2]); er += labs((long)out[i * 2 + 1]); }
    assert(er > 0);
    assert(el < er / 8);

    /* dual config + lane-R lock round-trip through the state blob */
    assert(smack_get_param(S, "state", snap, sizeof(snap)) > 0);
    assert(strstr(snap, "\"chan\":1"));
    assert(strstr(snap, "\"pan_l\":100"));
    assert(strstr(snap, "\"locks_r\":\"0:5:0\""));
    smack_set_param(S, "channel_mode", "0");
    smack_set_param(S, "pan_l", "0");
    smack_set_param(S, "lock_slice_r_0", "-1");
    smack_set_param(S, "state", snap);
    smack_get_param(S, "channel_mode", check, sizeof(check));
    assert(atoi(check) == 1);
    smack_get_param(S, "pattern_r", pr, sizeof(pr));
    assert(pr[0] == '0' + 5);              /* lane-R pin restored */

    /* monitor mute (feedback guard): passthrough silenced, ring still
     * records, loop playback stays audible */
    smack_get_param(S, "hw_input", check, sizeof(check));
    assert(atoi(check) == 0);              /* core default; gen wrapper sets 1 */
    smack_set_param(S, "hw_input", "1");
    smack_get_param(S, "hw_input", check, sizeof(check));
    assert(atoi(check) == 1);
    smack_set_param(S, "clear", "1");
    smack_set_param(S, "monitor", "0");
    run_blocks(120, out);
    assert(gp("run_state") == '0');
    assert(energy(out) == 0);              /* idle passthrough muted */
    smack_set_param(S, "capture", "1");    /* ring kept recording under mute */
    run_blocks(200, out);
    assert(gp("run_state") == '3');
    assert(energy(out) > 0);               /* loop playback audible while muted */
    smack_set_param(S, "monitor", "1");

    /* unlock_all: drops every pin on both lanes and rolls fresh */
    smack_set_param(S, "channel_mode", "1");
    smack_set_param(S, "seed", "888");
    smack_get_param(S, "pattern", patseed, sizeof(patseed));
    smack_set_param(S, "lock_slice_0", "5");
    smack_set_param(S, "lock_slice_r_2", "6");
    smack_get_param(S, "locked", buf, sizeof(buf));
    assert(buf[0] == '1');
    smack_get_param(S, "locked_r", buf, sizeof(buf));
    assert(buf[2] == '1');
    smack_set_param(S, "unlock_all", "1");
    smack_get_param(S, "locked", buf, sizeof(buf));
    assert(strchr(buf, '1') == NULL);          /* no pins left, lane L */
    smack_get_param(S, "locked_r", buf, sizeof(buf));
    assert(strchr(buf, '1') == NULL);          /* no pins left, lane R */
    smack_set_param(S, "seed", "888");         /* re-dial: canonical roll */
    smack_get_param(S, "pattern", buf, sizeof(buf));
    assert(strcmp(buf, patseed) == 0);         /* seeded pattern restored */

    /* BPM detection: beat-gated input at 120 BPM detects ~120 and applies
     * it as the free-run override; a steady tone reports none (0) */
    smack_set_param(S, "clear", "1");
    run_blocks_beat(3500, 120.0);              /* ~10 s of gated saw */
    smack_set_param(S, "detect_bpm", "1");
    {
        char db[16];
        int guard = 0;
        do {
            run_blocks(5, NULL);
            smack_get_param(S, "detected_bpm", db, sizeof(db));
        } while (atof(db) < 0.0 && ++guard < 200);
        double bpm = atof(db);
        assert(bpm > 114.0 && bpm < 126.0);
        smack_get_param(S, "bpm_override", db, sizeof(db));
        assert(atof(db) > 114.0 && atof(db) < 126.0);
    }
    /* capture a fresh loop (state went IDLE for the detection feed) so the
     * additive-mix and soft-assign checks run against real loop playback */
    smack_set_param(S, "channel_mode", "0");
    smack_set_param(S, "capture", "1");
    assert(gp("run_state") == '3');

    /* additive mix for input builds: with hw_input set, dry rides the
     * monitor switch at full level even at wet=100 (the chain build keeps
     * the crossfade, where monitor at wet=100 makes no difference) */
    {
        smack_set_param(S, "wet", "100");
        smack_set_param(S, "monitor", "0");
        run_blocks(30, out);
        long e0 = energy(out);
        smack_set_param(S, "monitor", "1");
        run_blocks(30, out);
        long e1 = energy(out);
        assert(e1 > e0);                       /* dry added on top of loop */

        /* the report that drove the redesign: monitor off + wet 0 must
         * still play the UNAFFECTED loop (wet blends clean<->pattern,
         * it is not a loop volume). The loop content is beat-gated, so
         * accumulate across >1 beat period to be sure we cross a burst. */
        smack_set_param(S, "wet", "0");
        smack_set_param(S, "monitor", "0");
        long etot = 0;
        for (int k = 0; k < 40; k++) { run_blocks(5, out); etot += energy(out); }
        assert(etot > 0);                      /* clean loop audible */
        smack_set_param(S, "monitor", "1");
        smack_set_param(S, "wet", "100");
    }

    /* soft assign (set_slice): changes the effect without pinning; the
     * next canonical roll replaces it */
    smack_set_param(S, "seed", "888");
    smack_get_param(S, "pattern", patseed, sizeof(patseed)); /* re-baseline */
    smack_set_param(S, "set_slice_0", "7");    /* bitcrush, soft */
    smack_get_param(S, "pattern", buf, sizeof(buf));
    assert(buf[0] == '0' + 7);
    smack_get_param(S, "locked", buf, sizeof(buf));
    assert(buf[0] == '0');                     /* NOT pinned */
    smack_set_param(S, "seed", "888");         /* canonical roll replaces it */
    smack_get_param(S, "pattern", buf, sizeof(buf));
    assert(strcmp(buf, patseed) == 0);

    smack_set_param(S, "bpm_override", "0");   /* back to project tempo */
    smack_set_param(S, "clear", "1");          /* IDLE: ring records again */
    run_blocks(3500, NULL);                    /* ~10 s of steady saw */
    smack_set_param(S, "detect_bpm", "trigger");
    {
        char db[16];
        int guard = 0;
        do {
            run_blocks(5, NULL);
            smack_get_param(S, "detected_bpm", db, sizeof(db));
        } while (atof(db) < 0.0 && ++guard < 200);
        assert(atof(db) == 0.0);               /* confidence gate: no beat */
    }

    /* a stopped clock must not swallow bpm_override (clock_seen is
     * sticky by design; the override outranks a non-running clock) */
    smack_set_param(S, "transport", "1");      /* Free: stop must not pause */
    smack_on_midi(S, &stop, 1, 3);             /* 0xFC: clock stops */
    smack_set_param(S, "bpm_override", "90");
    smack_set_param(S, "capture", "1");
    assert(gp("run_state") == '3');
    smack_get_param(S, "loop_frames", buf, sizeof(buf));
    /* 1 bar at 90 BPM = 44100 * 4 * 60/90 = 117600 frames */
    assert(atoi(buf) > 115000 && atoi(buf) < 120000);

    /* global punch: every effect renders under pressure sweeps; readback
     * works; -1 turns it off; punch survives the A side (forces effect) */
    smack_set_param(S, "capture", "1");
    assert(gp("run_state") == '3');
    for (int f = 0; f < SMACK_FX_COUNT; f++) {
        char v[8];
        snprintf(v, sizeof(v), "%d", f);
        smack_set_param(S, "punch_fx", v);
        smack_get_param(S, "punch_fx", buf, sizeof(buf));
        assert(atoi(buf) == f);
        for (int p = 0; p <= 127; p += 25) {
            snprintf(v, sizeof(v), "%d", p);
            smack_set_param(S, "punch_pressure", v);
            run_blocks(40, out);
        }
        assert(gp("run_state") == '3');
    }
    smack_set_param(S, "punch_fx", "-1");
    smack_get_param(S, "punch_fx", buf, sizeof(buf));
    assert(atoi(buf) == -1);

    /* ---- web-editor surface (v0.7.0) ---------------------------------- */

    /* lock_slice with "f:p": pin an effect WITH an explicit parameter */
    smack_set_param(S, "lock_slice_2", "3:-9");    /* pitch, -9 st */
    {
        char snap[32768];
        assert(smack_get_param(S, "state", snap, sizeof(snap)) > 0);
        assert(strstr(snap, "2:3:-9"));            /* serialized as i:f:p */
        smack_get_param(S, "pattern", buf, sizeof(buf));
        assert(buf[2] == '0' + 3);
        smack_get_param(S, "locked", buf, sizeof(buf));
        assert(buf[2] == '1');

        /* bare "f" still works (pad UIs), and display fields are present
         * and consistent: nsl matches n_slices, pat csv mirrors pattern */
        smack_set_param(S, "lock_slice_2", "-1");
        assert(smack_get_param(S, "state", snap, sizeof(snap)) > 0);
        assert(strstr(snap, "\"run\":3"));
        assert(strstr(snap, "\"pat\":\""));
        assert(strstr(snap, "\"fxp\":\""));
        assert(strstr(snap, "\"ord\":\""));
        char want[32];
        smack_get_param(S, "n_slices", buf, sizeof(buf));
        snprintf(want, sizeof(want), "\"nsl\":%d,", atoi(buf));
        assert(strstr(snap, want));
        /* first pat csv entry == first pattern char's fx code */
        const char *pp = strstr(snap, "\"pat\":\"") + 7;
        smack_get_param(S, "pattern", buf, sizeof(buf));
        assert(atoi(pp) == buf[0] - '0');
    }

    /* rui_poll digest: rev:on:tick:bpm; rev must move on a content edit */
    {
        unsigned r0, r1;
        int on, tick, bpm;
        smack_set_param(S, "rui_set", "wet:37");
        smack_get_param(S, "wet", buf, sizeof(buf));
        assert(atoi(buf) == 37);
        smack_set_param(S, "rui_set", "rui_set:wet:0");
        smack_get_param(S, "wet", buf, sizeof(buf));
        assert(atoi(buf) == 37);
        smack_set_param(S, "rui_set", "punch_fx:6:5:80:40");
        smack_get_param(S, "punch_fx", buf, sizeof(buf));
        assert(atoi(buf) == 6);                  /* routed values may contain ':' */
        smack_set_param(S, "rui_set", "punch_fx:-1");
        smack_set_param(S, "rui_set", "wet:100");
        smack_get_param(S, "rui_poll", buf, sizeof(buf));
        assert(sscanf(buf, "%u:%d:%d:%d", &r0, &on, &tick, &bpm) == 4);
        assert(on == 1);                           /* looping */
        assert(bpm > 85 && bpm < 95);              /* override 90 governs */
        smack_set_param(S, "reroll", "trigger");
        smack_get_param(S, "rui_poll", buf, sizeof(buf));
        assert(sscanf(buf, "%u:%d:%d:%d", &r1, &on, &tick, &bpm) == 4);
        assert(r1 != r0);                          /* edit bumped the rev */
        smack_get_param(S, "n_slices", buf, sizeof(buf));
        assert(tick >= 0 && tick < atoi(buf));
    }

    /* extended per-effect option space (v0.8.0): every effect renders
     * cleanly at its widest editor/punch parameter, both extremes */
    {
        static const int pmin[SMACK_FX_COUNT] =
            { 0, 1, 0, -24, 0, 0, 0, 2, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
              -24, 0, 0, 0 };
        static const int pmax[SMACK_FX_COUNT] =
            { 0, 6, 0,  24, 3, 3, 5, 24, 7, 7, 3, 0, 8, 7, 5, 7, 7, 7, 7, 7, 7, 7, 7,
               24, 7, 7, 7 };
        for (int f = 1; f < SMACK_FX_COUNT; f++) {
            char v[24];
            snprintf(v, sizeof(v), "%d:%d", f, pmax[f]);
            smack_set_param(S, "lock_slice_0", v);
            run_blocks(80, out);
            assert(gp("run_state") == '3');
            snprintf(v, sizeof(v), "%d:%d", f, pmin[f]);
            smack_set_param(S, "lock_slice_0", v);
            run_blocks(80, out);
            assert(gp("run_state") == '3');
            /* v0.12.0 depth + mix extremes render cleanly too */
            snprintf(v, sizeof(v), "%d:%d:100:50", f, pmax[f]);
            smack_set_param(S, "lock_slice_0", v);
            run_blocks(80, out);
            assert(gp("run_state") == '3');
            snprintf(v, sizeof(v), "%d:%d:0:1", f, pmin[f]);
            smack_set_param(S, "lock_slice_0", v);
            run_blocks(80, out);
            assert(gp("run_state") == '3');
        }
        smack_set_param(S, "lock_slice_0", "-1");
    }

    /* per-slice depth + mix (v0.12.0): extended lock tokens serialize as
     * i:f:p:p2:m, restore, and mix 0 really means "the clean slice" */
    {
        char snap[65536];
        smack_set_param(S, "lock_slice_3", "15:2:65:40"); /* filter, rez, 40% */
        assert(smack_get_param(S, "state", snap, sizeof(snap)) > 0);
        assert(strstr(snap, "3:15:2:65:40"));
        assert(strstr(snap, "\"fx2\":\""));
        assert(strstr(snap, "\"mix\":\""));

        /* restore round-trip keeps depth + mix */
        smack_set_param(S, "lock_slice_3", "-1");
        smack_set_param(S, "state", snap);
        assert(smack_get_param(S, "state", snap, sizeof(snap)) > 0);
        assert(strstr(snap, "3:15:2:65:40"));
        smack_set_param(S, "lock_slice_3", "-1");

        /* mix gate: ENV fade-out at full wet loses energy vs mix 0 */
        smack_set_param(S, "monitor", "0");   /* mute the live saw bleed */
        smack_set_param(S, "wet", "100");
        smack_set_param(S, "quantize", "0");
        smack_set_param(S, "ab", "1");            /* pattern side */
        smack_set_param(S, "order_density", "0");
        smack_set_param(S, "reroll", "trigger");
        smack_get_param(S, "n_slices", buf, sizeof(buf));
        int nsl = atoi(buf);
        char lk[24];
        for (int i = 0; i < nsl; i++) {
            char k[24];
            snprintf(k, sizeof(k), "lock_slice_%d", i);
            snprintf(lk, sizeof(lk), "13:1:-1:0");    /* fade-out, mix 0 */
            smack_set_param(S, k, lk);
        }
        long e_dry = 0, e_wet = 0;
        for (int k = 0; k < 40; k++) { run_blocks(5, out); e_dry += energy(out); }
        for (int i = 0; i < nsl; i++) {
            char k[24];
            snprintf(k, sizeof(k), "lock_slice_%d", i);
            smack_set_param(S, k, "13:1:-1:100");     /* fade-out, full wet */
        }
        for (int k = 0; k < 40; k++) { run_blocks(5, out); e_wet += energy(out); }
        assert(e_dry > 0);
        assert(e_wet < (e_dry * 3) / 4);  /* the envelope actually bites */
        smack_set_param(S, "unlock_all", "1");
        smack_set_param(S, "monitor", "1");   /* back to default */

        /* punch with a full token: variant + depth + mix */
        smack_set_param(S, "punch_fx", "6:5:80:40");
        smack_get_param(S, "punch_fx", buf, sizeof(buf));
        assert(atoi(buf) == 6);
        run_blocks(40, out);
        assert(gp("run_state") == '3');
        smack_set_param(S, "punch_fx", "-1");

        /* palette pads carry depth + mix: 4-field tokens round-trip */
        char pbuf[512];
        smack_set_param(S, "palette",
            "0,19:7:80:50,19:0,6:5,5,10,11,12,2,9,3,4,18,13,14,7,15,16,21,17,24,26,20");
        smack_get_param(S, "palette", pbuf, sizeof(pbuf));
        assert(strstr(pbuf, "19:7:80:50"));
        assert(smack_get_param(S, "state", snap, sizeof(snap)) > 0);
        assert(strstr(snap, "19:7:80:50"));
        smack_set_param(S, "palette",   /* factory again for later tests */
            "0,1,8,6,5,10,11,12,2,9,3,4,18,13,14,7,15,16,21,17,19,22,20");
    }

    /* state GET must never overflow a small caller buffer — snprintf
     * returns would-have-written lengths, so unclamped accumulation walks
     * past buf_len (the on-device host reads state into 16 KB; a 16-bar
     * dual-mono loop with heavy pins emits more than that) */
    {
        smack_set_param(S, "clear", "1");
        smack_set_param(S, "loop_len", "8");       /* 16 bars */
        smack_set_param(S, "slice_res", "0");      /* 1/2 step = 512 slices */
        smack_set_param(S, "channel_mode", "1");
        run_blocks(400, NULL);
        smack_set_param(S, "capture", "1");
        assert(gp("run_state") == '3');
        for (int i = 0; i < SMACK_MAX_SLICES; i++) {  /* max-size locks */
            char k[32];
            snprintf(k, sizeof(k), "lock_slice_%d", i);
            smack_set_param(S, k, "22:-100");
            snprintf(k, sizeof(k), "lock_slice_r_%d", i);
            smack_set_param(S, k, "22:-100");
        }
        static char big[65536];
        int sizes[3] = { 1024, 16384, 65536 };
        for (int z = 0; z < 3; z++) {
            memset(big, 0x7E, sizeof(big));
            int r = smack_get_param(S, "state", big, sizes[z]);
            assert(r >= 0 && r < sizes[z]);
            assert(big[sizes[z] - 1] == 0x7E ||
                   (r == sizes[z] - 1));            /* never past buf_len */
            assert(memchr(big, 0, (size_t)sizes[z]));  /* NUL-terminated */
        }
        smack_set_param(S, "unlock_all", "1");
        smack_set_param(S, "channel_mode", "0");
        smack_set_param(S, "loop_len", "4");
        smack_set_param(S, "slice_res", "1");
        smack_set_param(S, "clear", "1");
        run_blocks(1400, NULL);
        smack_set_param(S, "capture", "1");
        assert(gp("run_state") == '3');
    }

    /* layout apply: a partial state blob re-applies a pattern recipe
     * (seed/nonce/locks) without touching wet or the loop audio */
    {
        char snap[32768], pat_a[600];
        smack_set_param(S, "seed", "777");
        smack_set_param(S, "lock_slice_5", "7:6"); /* crush depth 6 */
        smack_get_param(S, "pattern", pat_a, sizeof(pat_a));
        assert(smack_get_param(S, "state", snap, sizeof(snap)) > 0);

        smack_set_param(S, "wet", "40");
        smack_set_param(S, "seed", "31337");       /* wander away */
        smack_set_param(S, "unlock_all", "1");
        smack_get_param(S, "pattern", buf, sizeof(buf));
        assert(strcmp(buf, pat_a) != 0);

        char part[4096];                           /* pattern recipe only */
        const char *lk = strstr(snap, "\"locks\":\"");
        assert(lk);
        char locks[2048];
        sscanf(lk + 9, "%2047[^\"]", locks);
        snprintf(part, sizeof(part),
                 "{\"seed\":777,\"nonce\":0,\"locks\":\"%s\"}", locks);
        smack_set_param(S, "state", part);
        smack_get_param(S, "pattern", buf, sizeof(buf));
        assert(strcmp(buf, pat_a) == 0);           /* exact pattern back */
        smack_get_param(S, "state", snap, sizeof(snap));
        assert(strstr(snap, "\"wet\":40"));        /* wet untouched */
        assert(strstr(snap, "5:7:6"));             /* lock w/ param back */
    }

    /* capture-while-looping grabs NEW audio (v0.9.0): the ring keeps
     * recording during playback instead of freezing, so a re-grab
     * re-slices what just played, not stale history */
    {
        smack_set_param(S, "clear", "1");
        run_blocks(1400, NULL);                 /* fresh saw into the ring */
        smack_set_param(S, "capture", "1");
        assert(gp("run_state") == '3');
        smack_set_param(S, "wet", "100");
        smack_set_param(S, "quantize", "0");
        smack_set_param(S, "ab", "0");          /* A side: clean loop only */
        smack_set_param(S, "monitor", "0");     /* no dry bleed */
        long e_saw = 0;
        for (int k = 0; k < 40; k++) { run_blocks(5, out); e_saw += energy(out); }
        assert(e_saw > 0);                      /* saw loop audible */

        /* feed silence for well over a loop while LOOPING, then re-grab */
        for (int k = 0; k < 1600; k++) run_blocks_lr(1, NULL, 0, 0);
        smack_set_param(S, "capture", "1");
        assert(gp("run_state") == '3');
        long e_sil = 0;
        for (int k = 0; k < 40; k++) { run_blocks_lr(5, out, 0, 0); e_sil += energy(out); }
        assert(e_sil < e_saw / 50);             /* grabbed the NEW (silent) audio */
    }

    /* pad-palette layout (v0.10.0, variants v0.11.0): 23 pads selecting
     * from the fx pool, each with an optional param override ("f:p").
     * Duplicates and subsets are legal; garbage never half-applies. */
    {
        char snap[32768], pbuf[256];
        smack_get_param(S, "palette", pbuf, sizeof(pbuf));
        assert(!strncmp(pbuf, "0,1,8,6,5,", 10));  /* factory order */

        /* swap positions 0 and 1, keep the rest */
        smack_set_param(S, "palette",
            "1,0,8,6,5,10,11,12,2,9,3,4,18,13,14,7,15,16,21,17,19,22,20");
        smack_get_param(S, "palette", pbuf, sizeof(pbuf));
        assert(!strncmp(pbuf, "1,0,8,", 6));

        /* rejected: short list, out-of-range code — layout keeps */
        smack_set_param(S, "palette", "5,4");
        smack_set_param(S, "palette",
            "27,0,8,6,5,10,11,12,2,9,3,4,18,13,14,7,15,16,21,17,19,22,1");
        smack_get_param(S, "palette", pbuf, sizeof(pbuf));
        assert(!strncmp(pbuf, "1,0,8,", 6));

        /* accepted: duplicates + variant pads + new fx codes (two Delay
         * pads at different times, a hot Buzz, PShift/RingMod/Scatter) */
        smack_set_param(S, "palette",
            "0,19:0,19:7,6:5,5,10,11,12,2,9,23:-12,4,18,13,14,7,15,16,21,17,24,26,20");
        smack_get_param(S, "palette", pbuf, sizeof(pbuf));
        assert(!strncmp(pbuf, "0,19:0,19:7,6:5,", 16));
        assert(strstr(pbuf, "23:-12"));

        /* survives a preset (state blob) round-trip */
        assert(smack_get_param(S, "state", snap, sizeof(snap)) > 0);
        assert(strstr(snap, "\"pal\":\"0,19:0,19:7,"));
        smack_set_param(S, "palette",
            "0,1,8,6,5,10,11,12,2,9,3,4,18,13,14,7,15,16,21,17,19,22,20");
        smack_set_param(S, "state", snap);
        smack_get_param(S, "palette", pbuf, sizeof(pbuf));
        assert(strstr(pbuf, "19:7"));

        /* old presets (no pal key) leave the arrangement alone */
        smack_set_param(S, "state", "{\"seed\":42}");
        smack_get_param(S, "palette", pbuf, sizeof(pbuf));
        assert(strstr(pbuf, "19:7"));

        /* punch_fx "f:p": a variant pad's punch starts from its param */
        smack_set_param(S, "punch_fx", "6:5");     /* Buzz — Massive */
        smack_get_param(S, "punch_fx", buf, sizeof(buf));
        assert(atoi(buf) == 6);
        run_blocks(40, out);
        assert(gp("run_state") == '3');
        smack_set_param(S, "punch_fx", "-1");

        /* set_slice "f:p": soft-assign with an explicit variant — the
         * state blob's pat/fxp csvs must show 20 (Dist) with param 7 */
        smack_set_param(S, "set_slice_1", "20:7"); /* Gnash hot */
        assert(smack_get_param(S, "state", snap, sizeof(snap)) > 0);
        const char *pc = strstr(snap, "\"pat\":\"");
        const char *fc = strstr(snap, "\"fxp\":\"");
        assert(pc && fc);
        pc = strchr(pc + 7, ',');                  /* second csv entry */
        fc = strchr(fc + 7, ',');
        assert(pc && fc);
        assert(atoi(pc + 1) == 20 && atoi(fc + 1) == 7);
    }

    /* pad-play (v0.13.0): MIDI notes trigger pattern cells, quantized to
     * pad_rate boundaries, repeating while held; release returns to the
     * loop. note % n_slices maps; velocity scales; last-note priority. */
    {
        char pb[16];
        smack_set_param(S, "clear", "1");
        smack_set_param(S, "bpm_override", "0"); /* clock ticks govern again */
        run_blocks(1400, NULL);
        smack_set_param(S, "capture", "1");    /* 1 bar, 16 slices @120 */
        assert(gp("run_state") == '3');
        smack_set_param(S, "monitor", "0");    /* energy = loop output only */
        smack_set_param(S, "wet", "100");
        smack_set_param(S, "ab", "0");         /* A side when not triggering */
        smack_set_param(S, "pad_play", "1");
        smack_set_param(S, "pad_rate", "2");   /* 1/4 = beat = 22050 frames */

        run_blocks(20, NULL);                  /* mid-beat, well off boundary */
        uint8_t non[3] = { 0x90, 60, 100 };    /* note 60 -> cell 60%16 = 12 */
        smack_on_midi(S, non, 3, 3);
        run_blocks(5, NULL);
        smack_get_param(S, "pad_state", pb, sizeof(pb));
        assert(pb[0] == '0');                  /* quantized: no instant fire */

        int waited = -1;                       /* fires at the beat boundary */
        for (int i = 0; i < 400; i++) {
            run_blocks(1, NULL);
            smack_get_param(S, "pad_state", pb, sizeof(pb));
            if (pb[0] == '1') { waited = i; break; }
        }
        assert(waited > 100);                  /* ~a beat away, not instant */
        assert(strcmp(pb, "1:12") == 0);       /* note % n_slices */
        run_blocks(2, out);
        assert(energy(out) > 0);               /* cell audible, vel-scaled */

        run_blocks(48, out);                   /* past the slice (43 blocks): */
        assert(energy(out) == 0);              /* between repeats = silence */
        run_blocks(130, out);                  /* next beat: repeat refires */
        smack_get_param(S, "pad_state", pb, sizeof(pb));
        assert(strcmp(pb, "1:12") == 0);
        assert(energy(out) > 0);

        uint8_t n65[3] = { 0x90, 65, 90 };     /* stack: 65 -> cell 1 on top */
        smack_on_midi(S, n65, 3, 3);
        for (int i = 0; i < 200; i++) {        /* next boundary switches cell */
            run_blocks(1, NULL);
            smack_get_param(S, "pad_state", pb, sizeof(pb));
            if (strcmp(pb, "1:1") == 0) break;
        }
        assert(strcmp(pb, "1:1") == 0);
        uint8_t off65[3] = { 0x80, 65, 0 };    /* release top: 60 still held */
        smack_on_midi(S, off65, 3, 3);
        smack_get_param(S, "pad_state", pb, sizeof(pb));
        assert(pb[0] == '1');
        for (int i = 0; i < 200; i++) {        /* falls back to note 60 */
            run_blocks(1, NULL);
            smack_get_param(S, "pad_state", pb, sizeof(pb));
            if (strcmp(pb, "1:12") == 0) break;
        }
        assert(strcmp(pb, "1:12") == 0);

        uint8_t off60[3] = { 0x80, 60, 0 };    /* full release -> loop back */
        smack_on_midi(S, off60, 3, 3);
        smack_get_param(S, "pad_state", pb, sizeof(pb));
        assert(pb[0] == '0');
        run_blocks(3, out);
        assert(energy(out) > 0);               /* clean loop resumed */

        /* pad_note param path (UI/web trigger) bypasses the pad_play gate */
        smack_set_param(S, "pad_play", "0");
        smack_set_param(S, "pad_note", "77:100");  /* 77%16 = cell 13 */
        for (int i = 0; i < 400; i++) {
            run_blocks(1, NULL);
            smack_get_param(S, "pad_state", pb, sizeof(pb));
            if (pb[0] == '1') break;
        }
        assert(strcmp(pb, "1:13") == 0);
        smack_set_param(S, "pad_note", "77:0");
        smack_get_param(S, "pad_state", pb, sizeof(pb));
        assert(pb[0] == '0');

        /* real notes ignored while pad_play off */
        smack_on_midi(S, non, 3, 3);
        run_blocks(200, NULL);
        smack_get_param(S, "pad_state", pb, sizeof(pb));
        assert(pb[0] == '0');

        /* settings ride the state blob; live note state never does */
        smack_set_param(S, "pad_play", "1");
        smack_set_param(S, "pad_rate", "4");
        char snap[32768];
        assert(smack_get_param(S, "state", snap, sizeof(snap)) > 0);
        assert(strstr(snap, "\"pp\":1"));
        assert(strstr(snap, "\"pr\":4"));
        smack_set_param(S, "state", "{\"pp\":0,\"pr\":2}");
        smack_get_param(S, "pad_play", pb, sizeof(pb));
        assert(pb[0] == '0');
        smack_get_param(S, "pad_rate", pb, sizeof(pb));
        assert(pb[0] == '2');
    }

    printf("host_sim: all assertions passed\n");
    smack_destroy(S);
    return 0;
}
