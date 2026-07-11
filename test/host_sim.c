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

int main(void) {
    host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.api_version = 1;
    host.sample_rate = SMACK_SR;
    host.frames_per_block = BLK;
    host.get_bpm = fake_bpm;

    S = smack_create(&host);
    assert(S);

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

    printf("host_sim: all assertions passed\n");
    smack_destroy(S);
    return 0;
}
