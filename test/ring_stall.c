/*
 * Native repro for the ring-recording stall — no hardware needed.
 *
 *   make test-ring
 *
 * Ported from smack-versio's firmware/test/test_ring_stall.c, where this bug
 * was found. The engine is shared, so the bug was always here too; the Versio
 * just hit it first because LIVE mode re-captures every loop pass and made the
 * staleness obvious.
 *
 * The bug:
 *
 *   record_ring_frame() refuses to write while SMACK_LOOPING once the write
 *   head comes within 2048 frames of the protected loop region. That guard is
 *   right in intent — the recorder must not overwrite the loop it is playing —
 *   but when it fires it also freezes ring_last_global, and capture_retro()
 *   clamps its capture point to that value. So every capture after the stall
 *   grabs a window ending at the frozen point. The module keeps looping and
 *   keeps accepting Capture; it just quietly returns audio from before the
 *   stall instead of what is being played now.
 *
 * Writable headroom after a capture is RING - loop_available, and
 * loop_available is never less than loop_len, so fresh audio can only
 * accumulate while loop_len < SMACK_RING_FRAMES / 2.
 *
 * On the Move that bites at ordinary tempos, not just slow ones. A half-step
 * is SMACK_SR * 7.5 / bpm frames, the longest LENGTH is 512 half-steps, and
 * the ring is 70 s, so the longest loop outgrows half the ring below about
 * 110 BPM. 100 BPM at max LENGTH — this test — is a 38.4 s loop against a
 * 70 s ring, leaving 31.6 s of writable space before the recorder jams.
 *
 * The fix steps LENGTH down to the longest setting that still fits, rather
 * than clipping the frame count (a clipped loop is no longer a whole number
 * of steps and would play back at the wrong rate).
 *
 * Staleness is measured from the OUTPUT, not from engine internals: the input
 * is DC that steps up over time, so "which era is in the loop" is readable by
 * playing the loop back as the clean loop tap (wet = 100, monitor = 0).
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/smack_core.h"

#define BLK 128
#define TEST_BPM 100.0f
#define LOOP_LEN_MAX_IDX 8      /* 512 half-steps, the longest LENGTH */

static float g_bpm = TEST_BPM;
static float host_get_bpm(void) { return g_bpm; }

typedef struct {
    smack_t *s;
    int16_t  in[BLK * 2];
    int16_t  out[BLK * 2];
} rig_t;

static int get_int(smack_t *s, const char *k) {
    char buf[64];
    if (smack_get_param(s, k, buf, sizeof(buf)) < 0) return -1;
    return atoi(buf);
}

static void rig_init(rig_t *r, float bpm, int loop_len_idx) {
    static host_api_v1_t host;

    g_bpm = bpm;
    memset(&host, 0, sizeof(host));
    host.api_version      = 1;
    host.sample_rate      = SMACK_SR;
    host.frames_per_block = BLK;
    host.get_bpm          = host_get_bpm;

    memset(r, 0, sizeof(*r));
    r->s = smack_create(&host);
    assert(r->s);

    /* The Move build scales the loop BY wet (out = loop*wet + in*dry), so the
     * clean loop tap is wet = 100 with monitor = 0 muting the live input.
     * smack-versio's rig uses wet = 0 for this -- its firmware mixes
     * differently, and copying that value here silently muted the loop. */
    smack_set_param(r->s, "hw_input", "1");
    smack_set_param(r->s, "monitor",  "0");
    smack_set_param(r->s, "wet",      "100");
    {
        char b[16];
        snprintf(b, sizeof(b), "%d", loop_len_idx);
        smack_set_param(r->s, "loop_len", b);
    }
}

/* Feed `secs` of DC at `level`, free-running (no MIDI clock). */
static void rig_run(rig_t *r, double secs, int level) {
    long blocks = (long)((secs * SMACK_SR) / BLK);
    for (int i = 0; i < BLK * 2; i++) r->in[i] = (int16_t)level;
    for (long b = 0; b < blocks; b++) smack_process(r->s, r->in, r->out, BLK);
}

/*
 * Play one full loop pass with a silent input and report what FRACTION of it
 * sits at or above `floor_val`.
 *
 * A fraction, not a peak. A stalled recorder is not frozen forever: each
 * capture moves the protected region and buys it a few seconds before it jams
 * again, so a stalled engine still lands a little fresh audio in the ring.
 * That was enough to make a max-abs check read "fresh". Asking how much of
 * the loop is fresh separates the two cleanly.
 */
static double loop_fraction_above(rig_t *r, int floor_val) {
    int  frames = get_int(r->s, "loop_frames");
    long blocks = frames > 0 ? (frames / BLK) : 0;
    long above = 0, total = 0;

    for (int i = 0; i < BLK * 2; i++) r->in[i] = 0;
    for (long b = 0; b < blocks; b++) {
        smack_process(r->s, r->in, r->out, BLK);
        for (int i = 0; i < BLK * 2; i++) {
            int v = r->out[i] < 0 ? -r->out[i] : r->out[i];
            if (v >= floor_val) above++;
            total++;
        }
    }
    return total > 0 ? (double)above / (double)total : 0.0;
}

/* Three levels, so "which era is in the loop" is unambiguous. ERA_MID plays
 * while the pre-fix recorder was still writing and ERA_NEW only after it
 * stalled — that is what makes this discriminate. */
#define ERA_PRE  3000
#define ERA_MID 12000
#define ERA_NEW 24000

/*
 * The captured loop must respect the half-ring rule.
 *
 * Cheap and direct: at 100 BPM the longest LENGTH wants 38.4 s against a
 * 34.9 s ceiling, so the engine has to step LENGTH down. Checked as an
 * invariant rather than against a hardcoded index, so it survives a change to
 * the length table or the margin.
 */
static void test_capture_fits_in_half_the_ring(void) {
    rig_t r;
    rig_init(&r, TEST_BPM, LOOP_LEN_MAX_IDX);

    rig_run(&r, 45.0, ERA_PRE);
    smack_set_param(r.s, "capture", "1");
    assert(get_int(r.s, "run_state") == 3);

    int frames = get_int(r.s, "loop_frames");
    int idx    = get_int(r.s, "loop_len");
    assert(frames > 0);
    /* Room for a whole fresh loop alongside the one playing. */
    assert((long)frames * 2 + 4096 <= (long)SMACK_RING_FRAMES);
    /* Stepped down a musical notch rather than clipping to a partial loop. */
    assert(idx < LOOP_LEN_MAX_IDX);

    printf("ok: capture at %g BPM fits the ring (LENGTH %d -> %d, %d frames)\n",
           (double)TEST_BPM, LOOP_LEN_MAX_IDX, idx, frames);
    smack_destroy(r.s);
}

/*
 * A manual Capture, pressed long after the previous one, must return audio
 * from now — not from before the recorder stopped.
 *
 * What the fix actually promises is bounded: a whole fresh loop's worth of
 * ring stays writable. So this asks for exactly that -- one loop-length of
 * new audio after the capture -- rather than the open-ended horizon
 * smack-versio's version uses, which does not hold on the Move.
 *
 * Measured both ways: 91.9% of the loop is the newest era after the fix,
 * 54.1% before it. The engine saturates at the stall point, so the figure is
 * stable rather than sensitive to how long the run is.
 */
static void test_capture_after_stall_is_fresh(void) {
    rig_t r;
    rig_init(&r, TEST_BPM, LOOP_LEN_MAX_IDX);

    rig_run(&r, 45.0, ERA_PRE);
    smack_set_param(r.s, "capture", "1");
    assert(get_int(r.s, "run_state") == 3);

    rig_run(&r, 24.0, ERA_NEW);   /* more than one loop-length of fresh audio */
    smack_set_param(r.s, "capture", "1");

    /* Not 100%: slice and loop edges fade over ~2.2 ms and dip below any
     * fixed floor. 0.9 clears that while still failing a mostly-stale loop. */
    double frac = loop_fraction_above(&r, (ERA_MID + ERA_NEW) / 2);
    printf("   %.1f%% of the loop is the newest era (want >90%%)\n",
           frac * 100.0);
    assert(frac > 0.9);

    printf("ok: capture long after the previous one returns current audio\n");
    smack_destroy(r.s);
}

int main(void) {
    test_capture_fits_in_half_the_ring();
    test_capture_after_stall_is_fresh();
    printf("ring_stall: all assertions passed\n");
    return 0;
}
