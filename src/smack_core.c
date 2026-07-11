/*
 * Smack core engine. See smack_core.h for the model.
 *
 * v1 simplifications (each is a documented follow-up, not an accident):
 *   - Ring recording pauses while LOOPING (no incremental copy-out yet), so
 *     a re-grab while looping re-slices the frozen pre-loop history.
 *   - Arm/record starts on a block boundary after the quantum tick, not
 *     sample-accurately inside the block (<= 2.9 ms early/late).
 *   - Retro grabs align to the last half-step boundary; bar-phase alignment
 *     for >= 1-bar grabs needs on-device verification of downbeat tracking.
 *   - Varispeed reads use linear interpolation; no anti-alias filter.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "smack_core.h"

/* Loop length menu, in half-steps (2 hs = 1 step, 32 hs = 1 bar in 4/4). */
static const int loop_len_hs_table[] = { 2, 4, 8, 16, 32, 64, 128, 256, 512 };
#define LOOP_LEN_COUNT 9
/* Slice resolution menu, in half-steps. */
static const int slice_hs_table[] = { 1, 2, 4, 8 };
#define SLICE_RES_COUNT 4

/* Retrig decay per repeat (0.85^k), k clamped to 15. */
static const float retrig_decay[16] = {
    1.0000f, 0.8500f, 0.7225f, 0.6141f, 0.5220f, 0.4437f, 0.3771f, 0.3206f,
    0.2725f, 0.2316f, 0.1969f, 0.1673f, 0.1422f, 0.1209f, 0.1028f, 0.0874f
};

/* RBJ biquad for the FILTER sweep effect */
typedef struct {
    float b0, b1, b2, a1, a2, z1, z2;
} smack_bq_t;

static void bq_reset(smack_bq_t *q) { q->z1 = q->z2 = 0.0f; }

static inline float bq_run(smack_bq_t *q, float x) {
    float y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

static void bq_set(smack_bq_t *q, float freq, int highpass) {
    const float Q = 0.9f;
    float w0 = 6.2831853f * freq / (float)SMACK_SR;
    float cw = cosf(w0), sw = sinf(w0);
    float alpha = sw / (2.0f * Q);
    float inv = 1.0f / (1.0f + alpha);
    if (highpass) {
        q->b0 = (1.0f + cw) * 0.5f * inv;
        q->b1 = -(1.0f + cw) * inv;
        q->b2 = q->b0;
    } else {
        q->b0 = (1.0f - cw) * 0.5f * inv;
        q->b1 = (1.0f - cw) * inv;
        q->b2 = q->b0;
    }
    q->a1 = -2.0f * cw * inv;
    q->a2 = (1.0f - alpha) * inv;
}

/* constant-peak bandpass, used by the vowel filter's formant bands */
static void bq_set_bandpass(smack_bq_t *q, float freq, float Q) {
    float w0 = 6.2831853f * freq / (float)SMACK_SR;
    float cw = cosf(w0), sw = sinf(w0);
    float alpha = sw / (2.0f * Q);
    float inv = 1.0f / (1.0f + alpha);
    q->b0 = alpha * inv;
    q->b1 = 0.0f;
    q->b2 = -alpha * inv;
    q->a1 = -2.0f * cw * inv;
    q->a2 = (1.0f - alpha) * inv;
}

/* Formant frequencies F1-F3 for A E I O U, and the morph pairs the VOWEL
 * effect sweeps across a slice (Looperator-style vowel transitions). */
static const float vowel_f[5][3] = {
    { 730.0f, 1090.0f, 2440.0f },  /* A */
    { 530.0f, 1840.0f, 2480.0f },  /* E */
    { 390.0f, 1990.0f, 2550.0f },  /* I */
    { 570.0f,  840.0f, 2410.0f },  /* O */
    { 440.0f, 1020.0f, 2240.0f },  /* U */
};
static const int vowel_pair[4][2] = { {0, 2}, {3, 2}, {0, 4}, {1, 3} }; /* A>I O>I A>U E>O */
static const float vowel_gain[3] = { 1.0f, 0.7f, 0.45f };

#define SMACK_DLY_LEN 8192  /* tonal-delay line, frames */

struct smack {
    const host_api_v1_t *host;

    /* Ring buffer — always recording input except while LOOPING. */
    int16_t *ring;               /* SMACK_RING_FRAMES * 2 samples */
    uint32_t ring_w;             /* write index, frames */
    uint64_t written_total;      /* frames ever written */
    uint64_t ring_last_global;   /* global frame of most recent written frame */

    /* Clock. Global frame counter runs across all blocks processed. */
    uint64_t global_frames;
    double   frames_per_tick;    /* smoothed; 918.75 = 120 BPM */
    uint64_t last_tick_global;   /* global frame of previous 0xF8 */
    uint64_t last_halfstep_global; /* global frame of last half-step boundary */
    uint32_t tick_total;         /* ticks since transport start */
    int      clock_running;
    int      clock_seen;

    /* Loop */
    smack_state_t state;
    uint32_t loop_start;         /* ring frame index */
    uint32_t loop_len;           /* frames */
    double   play_pos;           /* 0 .. loop_len */
    uint32_t rec_remaining;      /* frames left while RECORDING */
    uint32_t rec_start;          /* ring index where recording began */
    int      arm_start_flag;     /* set by tick handler: begin record next block */

    /* Params */
    int   loop_len_idx;          /* index into loop_len_hs_table */
    int   slice_res_idx;         /* index into slice_hs_table */
    float fx_density;            /* 0..1 */
    float order_density;         /* 0..1 */
    int   pitch_range;           /* 1..12 semitones */
    float wet;                   /* loop vs live input while LOOPING */
    int   ab;                    /* 0 = A clean, 1 = B pattern */
    int   ab_pending;            /* -1 none, else target value */
    int   quantize_mode;         /* 0 instant, 1 next slice, 2 next loop start */
    uint32_t seed;
    int   follow_transport;      /* 1 = Move stop pauses the loop (default) */
    int   transport_paused;      /* loop exists but transport is stopped */
    uint8_t last_trig[4];        /* change detection: capture, arm, reroll, clear */

    /* FILTER effect runtime */
    smack_bq_t fltL, fltR;
    int flt_slice;               /* output slice the filter is tracking, -1 none */
    int flt_counter;             /* frames until coefficient recompute */

    /* VOWEL effect runtime: 3 formant bands per channel */
    smack_bq_t vowL[3], vowR[3];
    int vow_slice;
    int vow_counter;

    /* TONALDELAY runtime: swept feedback delay line */
    float   *dly;                /* SMACK_DLY_LEN * 2 floats, interleaved */
    uint32_t dly_w;
    int      dly_slice;

    /* Pattern */
    int      n_slices;
    double   slice_frames;
    uint16_t order[SMACK_MAX_SLICES];
    uint8_t  fx[SMACK_MAX_SLICES];
    int8_t   fxp[SMACK_MAX_SLICES];
    uint8_t  fx_locked[SMACK_MAX_SLICES]; /* user-pinned: reroll keeps fx[i] */
    uint32_t rng;
};

/* ------------------------------------------------------------------ */
/*  RNG                                                                */
/* ------------------------------------------------------------------ */

static inline uint32_t xs32(uint32_t *st) {
    uint32_t x = *st;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *st = x;
    return x;
}
static inline float rnd01(uint32_t *st) { return (float)(xs32(st) >> 8) / 16777216.0f; }
static inline int rnd_below(uint32_t *st, int n) { return (int)(xs32(st) % (uint32_t)n); }

/* ------------------------------------------------------------------ */
/*  Clock helpers                                                      */
/* ------------------------------------------------------------------ */

static double frames_per_tick_now(smack_t *s) {
    if (s->clock_seen) return s->frames_per_tick;
    float bpm = 120.0f;
    if (s->host && s->host->get_bpm) {
        float b = s->host->get_bpm();
        if (b >= 20.0f && b <= 999.0f) bpm = b;
    }
    return (double)SMACK_SR * 60.0 / ((double)bpm * 24.0);
}

static double frames_per_halfstep(smack_t *s) { return frames_per_tick_now(s) * 3.0; }

/* Global frame of the most recent half-step boundary. */
static uint64_t last_boundary_global(smack_t *s) {
    if (s->clock_seen) return s->last_halfstep_global;
    double fph = frames_per_halfstep(s);
    return (uint64_t)(floor((double)s->global_frames / fph) * fph);
}

/* ------------------------------------------------------------------ */
/*  Pattern                                                            */
/* ------------------------------------------------------------------ */

static void roll_pattern(smack_t *s) {
    int loop_hs  = loop_len_hs_table[s->loop_len_idx];
    int slice_hs = slice_hs_table[s->slice_res_idx];
    int n = loop_hs / slice_hs;
    if (n < 1) n = 1;
    if (n > SMACK_MAX_SLICES) n = SMACK_MAX_SLICES;
    s->n_slices = n;
    s->slice_frames = (s->loop_len > 0) ? (double)s->loop_len / (double)n : 0.0;

    s->rng = s->seed ? s->seed : 0xC0FFEE01u;

    for (int i = 0; i < n; i++) s->order[i] = (uint16_t)i;
    for (int i = 0; i < n; i++) {
        if (rnd01(&s->rng) < s->order_density) {
            int j = rnd_below(&s->rng, n);
            uint16_t t = s->order[i]; s->order[i] = s->order[j]; s->order[j] = t;
        }
    }

    for (int i = 0; i < n; i++) {
        if (s->fx_locked[i]) continue;
        if (rnd01(&s->rng) < s->fx_density) {
            int f = 1 + rnd_below(&s->rng, SMACK_FX_COUNT - 1);
            s->fx[i] = (uint8_t)f;
            switch (f) {
            case SMACK_FX_RETRIG: s->fxp[i] = (int8_t)(1 + rnd_below(&s->rng, 3)); break;
            case SMACK_FX_PITCH: {
                int semi = 1 + rnd_below(&s->rng, s->pitch_range);
                if (xs32(&s->rng) & 1) semi = -semi;
                s->fxp[i] = (int8_t)semi;
                break;
            }
            case SMACK_FX_SPEED: s->fxp[i] = (int8_t)(xs32(&s->rng) & 1); break;
            case SMACK_FX_BUZZ:  s->fxp[i] = (int8_t)(xs32(&s->rng) & 3); break;
            case SMACK_FX_CRUSH: s->fxp[i] = (int8_t)(2 + (xs32(&s->rng) & 6)); break;
            case SMACK_FX_REPEAT:
            case SMACK_FX_REVAFTER: s->fxp[i] = (int8_t)(1 + rnd_below(&s->rng, 3)); break;
            case SMACK_FX_TAPESTOP: s->fxp[i] = (int8_t)(xs32(&s->rng) & 1); break;
            case SMACK_FX_SCRATCH:  s->fxp[i] = (int8_t)(1 + rnd_below(&s->rng, 3)); break;
            case SMACK_FX_ENV:      s->fxp[i] = (int8_t)rnd_below(&s->rng, 4); break;
            case SMACK_FX_PAN:      s->fxp[i] = (int8_t)rnd_below(&s->rng, 3); break;
            case SMACK_FX_FILTER:   s->fxp[i] = (int8_t)rnd_below(&s->rng, 4); break;
            case SMACK_FX_VOWEL:    s->fxp[i] = (int8_t)rnd_below(&s->rng, 4); break;
            case SMACK_FX_TONALDELAY: s->fxp[i] = (int8_t)rnd_below(&s->rng, 4); break;
            case SMACK_FX_FREEZE:   s->fxp[i] = (int8_t)rnd_below(&s->rng, 4); break;
            default:             s->fxp[i] = 0; break;
            }
        } else {
            s->fx[i] = SMACK_FX_NONE;
            s->fxp[i] = 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Capture                                                            */
/* ------------------------------------------------------------------ */

static uint32_t quantum_frames(smack_t *s) {
    double f = frames_per_halfstep(s) * (double)loop_len_hs_table[s->loop_len_idx];
    if (f < 256.0) f = 256.0;
    if (f > (double)SMACK_RING_FRAMES) f = (double)SMACK_RING_FRAMES;
    return (uint32_t)f;
}

/* Retroactive grab: the last loop-length of audio ending at the most recent
 * half-step boundary becomes the loop. Uses the written-frame timeline, so a
 * grab while LOOPING re-slices the frozen history (documented limitation). */
static void capture_retro(smack_t *s) {
    uint32_t want = quantum_frames(s);
    uint64_t boundary = last_boundary_global(s);
    if (boundary > s->ring_last_global) boundary = s->ring_last_global;
    uint64_t behind = s->ring_last_global - boundary; /* frames since boundary */
    if (behind > (uint64_t)SMACK_RING_FRAMES) behind = 0;

    uint64_t avail = s->written_total;
    if (avail > (uint64_t)SMACK_RING_FRAMES) avail = SMACK_RING_FRAMES;
    if (behind + want > avail) {
        if (avail <= behind + 256) return; /* nothing meaningful recorded yet */
        want = (uint32_t)(avail - behind);
    }

    uint64_t frames_ago_start = behind + want;
    s->loop_len   = want;
    s->loop_start = (uint32_t)((s->ring_w + (uint64_t)SMACK_RING_FRAMES * 2
                                - frames_ago_start) % SMACK_RING_FRAMES);
    s->play_pos = 0.0;
    s->state = SMACK_LOOPING;
    roll_pattern(s);
}

static void begin_record(smack_t *s) {
    s->rec_start = s->ring_w;
    s->rec_remaining = quantum_frames(s);
    s->state = SMACK_RECORDING;
}

static void finish_record(smack_t *s) {
    s->loop_start = s->rec_start;
    s->loop_len   = quantum_frames(s);
    s->play_pos = 0.0;
    s->state = SMACK_LOOPING;
    roll_pattern(s);
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                          */
/* ------------------------------------------------------------------ */

smack_t *smack_create(const host_api_v1_t *host) {
    smack_t *s = calloc(1, sizeof(smack_t));
    if (!s) return NULL;
    s->ring = calloc((size_t)SMACK_RING_FRAMES * 2, sizeof(int16_t));
    if (!s->ring) { free(s); return NULL; }
    s->dly = calloc((size_t)SMACK_DLY_LEN * 2, sizeof(float));
    if (!s->dly) { free(s->ring); free(s); return NULL; }
    s->host = host;
    s->frames_per_tick = 918.75; /* 120 BPM */
    s->loop_len_idx = 4;         /* 1 bar */
    s->slice_res_idx = 1;        /* 1 step */
    s->fx_density = 0.5f;
    s->order_density = 0.0f;
    s->pitch_range = 12;
    s->wet = 1.0f;
    s->ab = 1;
    s->ab_pending = -1;
    s->quantize_mode = 1;        /* next slice */
    s->seed = 0x5EEDu;
    s->follow_transport = 1;
    s->flt_slice = -1;
    s->vow_slice = -1;
    s->dly_slice = -1;
    memset(s->last_trig, 0xFF, sizeof(s->last_trig));
    return s;
}

/* Trigger params ride enum knobs whose engine caches its own position, so a
 * knob parked on "Grab" never re-sends the same value. Fire on the FIRST
 * non-zero value, then on EVERY change — wiggling the knob back and forth
 * fires each detent, and autosave restores (a single value at load) don't. */
static int trig_fired(smack_t *s, int idx, const char *val) {
    int v = atoi(val) ? 1 : 0;
    int fired = (s->last_trig[idx] == 0xFF) ? v : (v != s->last_trig[idx]);
    s->last_trig[idx] = (uint8_t)v;
    return fired;
}

static int json_int(const char *js, const char *key, int def) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(js, pat);
    return p ? atoi(p + strlen(pat)) : def;
}

void smack_destroy(smack_t *s) {
    if (!s) return;
    free(s->dly);
    free(s->ring);
    free(s);
}

/* ------------------------------------------------------------------ */
/*  MIDI (clock + transport)                                           */
/* ------------------------------------------------------------------ */

void smack_on_midi(smack_t *s, const uint8_t *msg, int len, int source) {
    (void)source;
    if (!s || len < 1) return;
    switch (msg[0]) {
    case 0xFA: /* start */
    case 0xFB: /* continue: treated as downbeat too (pushnpull convention) */
        s->tick_total = 0;
        s->clock_running = 1;
        s->last_halfstep_global = s->global_frames;
        if (s->transport_paused) { /* resume the loop from its top */
            s->transport_paused = 0;
            s->play_pos = 0.0;
        }
        break;
    case 0xFC:
        s->clock_running = 0;
        if (s->follow_transport && s->state == SMACK_LOOPING)
            s->transport_paused = 1;
        break;
    case 0xF8: {
        if (s->clock_seen && s->global_frames > s->last_tick_global) {
            double d = (double)(s->global_frames - s->last_tick_global);
            if (d > 100.0 && d < 20000.0)
                s->frames_per_tick = 0.9 * s->frames_per_tick + 0.1 * d;
        }
        s->last_tick_global = s->global_frames;
        s->clock_seen = 1;
        s->tick_total++;
        if (s->tick_total % 3 == 0) { /* half-step boundary */
            s->last_halfstep_global = s->global_frames;
            if (s->state == SMACK_ARMED && s->tick_total % 6 == 0)
                s->arm_start_flag = 1; /* start on step boundaries */
        }
        break;
    }
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/*  Rendering                                                          */
/* ------------------------------------------------------------------ */

static inline void ring_read_lerp(const smack_t *s, double loop_off,
                                  float *l, float *r) {
    if (loop_off < 0.0) loop_off = 0.0;
    double maxo = (double)s->loop_len - 1.0;
    if (loop_off > maxo) loop_off = maxo;
    uint32_t i0 = (uint32_t)loop_off;
    float frac = (float)(loop_off - (double)i0);
    uint32_t a = (s->loop_start + i0) % SMACK_RING_FRAMES;
    uint32_t b = (i0 + 1 <= (uint32_t)maxo)
                   ? (s->loop_start + i0 + 1) % SMACK_RING_FRAMES : a;
    *l = (float)s->ring[a * 2]     + frac * ((float)s->ring[b * 2]     - (float)s->ring[a * 2]);
    *r = (float)s->ring[a * 2 + 1] + frac * ((float)s->ring[b * 2 + 1] - (float)s->ring[a * 2 + 1]);
}

static inline int16_t clip16(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)v;
}

/* Render one looped output frame at play_pos into l/r. */
static void render_loop_frame(smack_t *s, float *l, float *r) {
    double sf = s->slice_frames;
    int oslice = (sf > 0.0) ? (int)(s->play_pos / sf) : 0;
    if (oslice >= s->n_slices) oslice = s->n_slices - 1;
    double p = s->play_pos - (double)oslice * sf;
    float g = 1.0f, gl = 1.0f, gr = 1.0f;
    int f = SMACK_FX_NONE, fp = 0;
    double src;
    double src2 = -1.0;   /* FREEZE: second grain tap (slice-relative) */
    float  mix2 = 0.0f;   /* FREEZE: crossfade toward primary tap */

    if (!s->ab) {
        src = s->play_pos;                      /* A: clean, original order */
    } else {
        int sslice = s->order[oslice];
        f  = s->fx[oslice];
        fp = s->fxp[oslice];
        double rp = p;
        switch (f) {
        case SMACK_FX_RETRIG: {
            double win = sf / (double)(1 << fp);
            if (win < 32.0) win = 32.0;
            int k = (int)(p / win);
            rp = p - (double)k * win;
            g *= retrig_decay[k > 15 ? 15 : k];
            break;
        }
        case SMACK_FX_REVERSE: rp = (sf - 1.0) - p; break;
        case SMACK_FX_PITCH: {
            double rate = pow(2.0, (double)fp / 12.0);
            rp = fmod(p * rate, sf);
            break;
        }
        case SMACK_FX_SPEED:
            rp = fp ? fmod(p * 2.0, sf) : p * 0.5;
            break;
        case SMACK_FX_GATE: {
            int seg = (int)(p * 8.0 / sf);
            if (seg & 1) g *= 0.12f;
            break;
        }
        case SMACK_FX_BUZZ: {
            double win = (double)(96 << (fp & 3));
            rp = fmod(p, win);
            break;
        }
        case SMACK_FX_CRUSH: {
            double hold = (double)fp;
            rp = floor(p / hold) * hold;
            break;
        }
        case SMACK_FX_REPEAT: {
            /* play the head, then stutter it (Looperator "repeat after X") */
            static const double frac[4] = { 0.5, 0.5, 0.25, 0.75 };
            double split = sf * frac[fp & 3];
            rp = (p < split) ? p : fmod(p - split, split);
            break;
        }
        case SMACK_FX_REVAFTER: {
            /* forward, then play backwards from the split point */
            static const double frac[4] = { 0.5, 0.5, 0.6667, 0.75 };
            double split = sf * frac[fp & 3];
            if (p < split) rp = p;
            else {
                rp = split - (p - split);
                if (rp < 0.0) rp = 0.0;
            }
            break;
        }
        case SMACK_FX_TAPESTOP: {
            /* rate ramps 1 -> 0; position is the integral of the ramp */
            if (fp) { /* fast: dead by half-slice */
                rp = (p < sf * 0.5) ? p - p * p / sf : sf * 0.25;
                if (p >= sf * 0.5) g = 0.0f;
            } else {
                rp = p - p * p / (2.0 * sf);
            }
            break;
        }
        case SMACK_FX_TAPESTART:
            rp = p * p / (2.0 * sf);            /* spin up from standstill */
            break;
        case SMACK_FX_SCRATCH: {
            double cyc = (double)(fp & 3);
            double depth = sf / 10.0;
            rp = p + depth * sin(6.2831853 * cyc * p / sf);
            break;
        }
        case SMACK_FX_ENV:
            switch (fp & 3) {
            case 0: g *= (float)(p / sf); break;                 /* fade in */
            case 1: g *= (float)(1.0 - p / sf); break;           /* fade out */
            case 2: g *= 0.5f + 0.5f * sinf((float)(6.2831853 * 6.0 * p / sf)); break;
            case 3: if (p < sf * 0.5) g = 0.0f; break;           /* off-then-on */
            }
            break;
        case SMACK_FX_PAN:
            switch (fp % 3) {
            case 0: gr = 0.08f; break;                           /* hard left */
            case 1: gl = 0.08f; break;                           /* hard right */
            case 2: {                                            /* ping-pong x4 */
                int seg = (int)(p * 4.0 / sf);
                if (seg & 1) gl = 0.08f; else gr = 0.08f;
                break;
            }
            }
            break;
        case SMACK_FX_FREEZE: {
            /* 50%-overlap grains frozen on the slice head; per-grain start
             * jitter (deterministic from grain index + seed) sprays the
             * texture. Two taps, triangular crossfade. */
            const double H = 512.0;                 /* half grain (~12 ms) */
            double region = sf * 0.25 - 2.0 * H;
            if (region < 0.0) region = 0.0;
            double spray = region * (double)((fp & 3) + 1) * 0.25;
            int a = (int)(p / H);
            double local = p - (double)a * H;
            uint32_t h1 = ((uint32_t)a * 2654435761u) ^ s->seed;
            uint32_t h0 = ((uint32_t)(a - 1) * 2654435761u) ^ s->seed;
            rp = (double)(h1 & 0xFFFF) / 65535.0 * spray + local;
            src2 = (double)(h0 & 0xFFFF) / 65535.0 * spray + H + local;
            mix2 = (float)(local / H);              /* 0 -> old tap, 1 -> new */
            break;
        }
        default: break;
        }
        if (rp < 0.0) rp = 0.0;
        if (rp > sf - 1.0) rp = sf - 1.0;
        src = (double)sslice * sf + rp;
        if (src2 >= 0.0) {
            if (src2 > sf - 1.0) src2 = sf - 1.0;
            src2 = (double)sslice * sf + src2;
        }

        /* slice-edge fade masks discontinuities from reorder/fx */
        if (p < SMACK_EDGE_FADE) g *= (float)(p / SMACK_EDGE_FADE);
        double tail = sf - p;
        if (tail < SMACK_EDGE_FADE) g *= (float)(tail / SMACK_EDGE_FADE);
    }

    /* loop-boundary fade in both modes */
    if (s->play_pos < SMACK_EDGE_FADE)
        g *= (float)(s->play_pos / SMACK_EDGE_FADE);
    double ltail = (double)s->loop_len - s->play_pos;
    if (ltail < SMACK_EDGE_FADE) g *= (float)(ltail / SMACK_EDGE_FADE);

    ring_read_lerp(s, src, l, r);

    if (src2 >= 0.0) { /* FREEZE: blend the second grain tap */
        float l2, r2;
        ring_read_lerp(s, src2, &l2, &r2);
        *l = *l * mix2 + l2 * (1.0f - mix2);
        *r = *r * mix2 + r2 * (1.0f - mix2);
    }

    if (f == SMACK_FX_FILTER) {
        if (s->flt_slice != oslice) { /* fresh sweep on slice entry */
            bq_reset(&s->fltL);
            bq_reset(&s->fltR);
            s->flt_slice = oslice;
            s->flt_counter = 0;
        }
        if (s->flt_counter-- <= 0) {
            s->flt_counter = 32;
            float t = (float)(p / sf);
            float fc;
            int hp = (fp & 3) >= 2;
            switch (fp & 3) {
            case 0:  fc = 6000.0f * powf(200.0f / 6000.0f, t); break; /* LP down */
            case 1:  fc = 200.0f * powf(6000.0f / 200.0f, t); break;  /* LP up */
            case 2:  fc = 120.0f * powf(4000.0f / 120.0f, t); break;  /* HP up */
            default: fc = 4000.0f * powf(120.0f / 4000.0f, t); break; /* HP down */
            }
            bq_set(&s->fltL, fc, hp);
            s->fltR = s->fltL;
        }
        *l = bq_run(&s->fltL, *l);
        *r = bq_run(&s->fltR, *r);
    } else {
        s->flt_slice = -1;
    }

    if (f == SMACK_FX_VOWEL) {
        if (s->vow_slice != oslice) {
            for (int k = 0; k < 3; k++) { bq_reset(&s->vowL[k]); bq_reset(&s->vowR[k]); }
            s->vow_slice = oslice;
            s->vow_counter = 0;
        }
        if (s->vow_counter-- <= 0) {
            s->vow_counter = 32;
            float t = (float)(p / sf);
            const int *pr = vowel_pair[fp & 3];
            for (int k = 0; k < 3; k++) {
                float fc = vowel_f[pr[0]][k] + (vowel_f[pr[1]][k] - vowel_f[pr[0]][k]) * t;
                bq_set_bandpass(&s->vowL[k], fc, 8.0f);
                s->vowR[k] = s->vowL[k];
            }
        }
        float xl = *l, xr = *r, ol = 0.0f, orr = 0.0f;
        for (int k = 0; k < 3; k++) {
            ol  += vowel_gain[k] * bq_run(&s->vowL[k], xl);
            orr += vowel_gain[k] * bq_run(&s->vowR[k], xr);
        }
        *l = ol * 2.4f;
        *r = orr * 2.4f;
    } else {
        s->vow_slice = -1;
    }

    if (f == SMACK_FX_TONALDELAY) {
        if (s->dly_slice != oslice) { /* fresh line per slice entry */
            memset(s->dly, 0, (size_t)SMACK_DLY_LEN * 2 * sizeof(float));
            s->dly_w = 0;
            s->dly_slice = oslice;
        }
        double t = p / sf;
        const double dmax = 3000.0, dmin = 700.0;
        double d;
        switch (fp & 3) {
        case 0:  d = dmax - (dmax - dmin) * t; break;   /* repeats pitch up */
        case 1:  d = dmin + (dmax - dmin) * t; break;   /* repeats pitch down */
        case 2:  d = dmin + (dmax - dmin) * 0.5 * (1.0 + sin(6.2831853 * 2.0 * t)); break;
        default: d = dmax - (dmax - dmin) * (floor(t * 4.0) * 0.25); break; /* stepped */
        }
        double rpos = (double)s->dly_w - d;
        while (rpos < 0.0) rpos += (double)SMACK_DLY_LEN;
        uint32_t i0 = (uint32_t)rpos % SMACK_DLY_LEN;
        uint32_t i1 = (i0 + 1) % SMACK_DLY_LEN;
        float fr2 = (float)(rpos - floor(rpos));
        float dl = s->dly[i0 * 2]     + fr2 * (s->dly[i1 * 2]     - s->dly[i0 * 2]);
        float dr = s->dly[i0 * 2 + 1] + fr2 * (s->dly[i1 * 2 + 1] - s->dly[i0 * 2 + 1]);
        s->dly[s->dly_w * 2]     = *l * 0.55f + dl * 0.5f;
        s->dly[s->dly_w * 2 + 1] = *r * 0.55f + dr * 0.5f;
        s->dly_w = (s->dly_w + 1) % SMACK_DLY_LEN;
        *l = *l * 0.6f + dl * 0.85f;
        *r = *r * 0.6f + dr * 0.85f;
    } else {
        s->dly_slice = -1;
    }

    *l *= g * gl;
    *r *= g * gr;

    if (f == SMACK_FX_CRUSH) {
        *l = (float)(((int)*l >> 5) << 5);
        *r = (float)(((int)*r >> 5) << 5);
    }
}

void smack_process(smack_t *s, const int16_t *in, int16_t *out, int frames) {
    if (!s) return;

    if (s->state == SMACK_ARMED && s->arm_start_flag) {
        s->arm_start_flag = 0;
        begin_record(s);
    }

    for (int n = 0; n < frames; n++) {
        float inl = (float)in[n * 2], inr = (float)in[n * 2 + 1];

        if (s->state != SMACK_LOOPING || s->transport_paused) {
            /* record + pass through */
            s->ring[s->ring_w * 2]     = in[n * 2];
            s->ring[s->ring_w * 2 + 1] = in[n * 2 + 1];
            s->ring_w = (s->ring_w + 1) % SMACK_RING_FRAMES;
            s->written_total++;
            s->ring_last_global = s->global_frames + (uint64_t)n;

            if (s->state == SMACK_RECORDING && s->rec_remaining > 0) {
                if (--s->rec_remaining == 0) finish_record(s);
            }
            out[n * 2]     = clip16(inl);
            out[n * 2 + 1] = clip16(inr);
        } else {
            /* quantized A/B switch */
            if (s->ab_pending >= 0) {
                int apply = 0;
                if (s->quantize_mode == 0) apply = 1;
                else if (s->quantize_mode == 1) {
                    double p = fmod(s->play_pos, s->slice_frames);
                    if (p < 1.0) apply = 1;
                } else if (s->play_pos < 1.0) apply = 1;
                if (apply) { s->ab = s->ab_pending; s->ab_pending = -1; }
            }

            float ll, rr;
            render_loop_frame(s, &ll, &rr);
            out[n * 2]     = clip16(ll * s->wet + inl * (1.0f - s->wet));
            out[n * 2 + 1] = clip16(rr * s->wet + inr * (1.0f - s->wet));

            s->play_pos += 1.0;
            if (s->play_pos >= (double)s->loop_len) s->play_pos = 0.0;
        }
    }
    s->global_frames += (uint64_t)frames;
}

/* ------------------------------------------------------------------ */
/*  Params                                                             */
/* ------------------------------------------------------------------ */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void smack_set_param(smack_t *s, const char *key, const char *val) {
    if (!s || !key || !val) return;
    if (!strcmp(key, "loop_len")) {
        s->loop_len_idx = clampi(atoi(val), 0, LOOP_LEN_COUNT - 1);
    } else if (!strcmp(key, "slice_res")) {
        s->slice_res_idx = clampi(atoi(val), 0, SLICE_RES_COUNT - 1);
        if (s->state == SMACK_LOOPING) roll_pattern(s);
    } else if (!strcmp(key, "fx_density")) {
        s->fx_density = (float)atof(val) / 100.0f;
        if (s->state == SMACK_LOOPING) roll_pattern(s);
    } else if (!strcmp(key, "order_density")) {
        s->order_density = (float)atof(val) / 100.0f;
        if (s->state == SMACK_LOOPING) roll_pattern(s);
    } else if (!strcmp(key, "pitch_range")) {
        s->pitch_range = clampi(atoi(val), 1, 12);
    } else if (!strcmp(key, "wet")) {
        s->wet = (float)atof(val) / 100.0f;
    } else if (!strcmp(key, "ab")) {
        int v = atoi(val) ? 1 : 0;
        if (s->state == SMACK_LOOPING && s->quantize_mode != 0) s->ab_pending = v;
        else s->ab = v;
    } else if (!strcmp(key, "quantize")) {
        s->quantize_mode = clampi(atoi(val), 0, 2);
    } else if (!strcmp(key, "seed")) {
        /* Seed is the pattern's ID: dialing it browses patterns directly,
         * and returning to a number restores that exact pattern. */
        s->seed = (uint32_t)strtoul(val, NULL, 10);
        if (s->state == SMACK_LOOPING) roll_pattern(s);
    } else if (!strcmp(key, "reroll")) {
        if (trig_fired(s, 2, val)) {
            s->seed = s->seed * 1664525u + 1013904223u;
            if (s->state == SMACK_LOOPING) roll_pattern(s);
        }
    } else if (!strcmp(key, "capture")) {
        if (trig_fired(s, 0, val)) {
            capture_retro(s);
            s->transport_paused = 0; /* manual grab plays even when stopped */
        }
    } else if (!strcmp(key, "arm")) {
        if (trig_fired(s, 1, val) &&
            (s->state == SMACK_IDLE || s->state == SMACK_LOOPING)) {
            s->state = SMACK_ARMED;
            s->transport_paused = 0;
            s->arm_start_flag = !s->clock_running; /* free-run: start next block */
        }
    } else if (!strcmp(key, "clear")) {
        if (trig_fired(s, 3, val)) {
            s->state = SMACK_IDLE;
            s->ab_pending = -1;
            s->transport_paused = 0;
            memset(s->fx_locked, 0, sizeof(s->fx_locked));
        }
    } else if (!strcmp(key, "transport")) {
        s->follow_transport = atoi(val) ? 0 : 1; /* 0 Follow, 1 Free */
        if (!s->follow_transport) s->transport_paused = 0;
    } else if (!strcmp(key, "state")) {
        /* preset/autosave restore: settings + slice locks, never audio */
        s->loop_len_idx  = clampi(json_int(val, "loop_len", s->loop_len_idx), 0, LOOP_LEN_COUNT - 1);
        s->slice_res_idx = clampi(json_int(val, "slice_res", s->slice_res_idx), 0, SLICE_RES_COUNT - 1);
        s->fx_density    = (float)clampi(json_int(val, "fx_density", (int)(s->fx_density * 100.0f)), 0, 100) / 100.0f;
        s->order_density = (float)clampi(json_int(val, "order_density", (int)(s->order_density * 100.0f)), 0, 100) / 100.0f;
        s->pitch_range   = clampi(json_int(val, "pitch_range", s->pitch_range), 1, 12);
        s->wet           = (float)clampi(json_int(val, "wet", (int)(s->wet * 100.0f)), 0, 100) / 100.0f;
        s->ab            = json_int(val, "ab", s->ab) ? 1 : 0;
        s->quantize_mode = clampi(json_int(val, "quantize", s->quantize_mode), 0, 2);
        s->seed          = (uint32_t)json_int(val, "seed", (int)s->seed);
        s->follow_transport = json_int(val, "transport", s->follow_transport ? 0 : 1) ? 0 : 1;
        memset(s->fx_locked, 0, sizeof(s->fx_locked));
        const char *lk = strstr(val, "\"locks\":\"");
        if (lk) {
            lk += 9;
            while (*lk && *lk != '"') {
                char *end;
                long i = strtol(lk, &end, 10);
                if (end == lk || *end != ':') break;
                long f = strtol(end + 1, &end, 10);
                if (i >= 0 && i < SMACK_MAX_SLICES) {
                    s->fx_locked[i] = 1;
                    s->fx[i] = (uint8_t)clampi((int)f, 0, SMACK_FX_COUNT - 1);
                }
                if (*end == ',') lk = end + 1; else break;
            }
        }
        if (s->state == SMACK_LOOPING) roll_pattern(s);
    } else if (!strncmp(key, "lock_slice_", 11)) {
        int i = clampi(atoi(key + 11), 0, SMACK_MAX_SLICES - 1);
        int f = atoi(val);
        if (f < 0) { /* unlock: re-roll restores the seeded value */
            s->fx_locked[i] = 0;
            if (s->state == SMACK_LOOPING) roll_pattern(s);
        } else {
            s->fx[i] = (uint8_t)clampi(f, 0, SMACK_FX_COUNT - 1);
            s->fx_locked[i] = 1;
        }
    }
}

int smack_get_param(smack_t *s, const char *key, char *buf, int buf_len) {
    if (!s || !key || !buf || buf_len < 2) return -1;
    if (!strcmp(key, "run_state")) /* machine state: 0 idle..3 looping */
        return snprintf(buf, (size_t)buf_len, "%d", (int)s->state);
    if (!strcmp(key, "loop_len"))
        return snprintf(buf, (size_t)buf_len, "%d", s->loop_len_idx);
    if (!strcmp(key, "slice_res"))
        return snprintf(buf, (size_t)buf_len, "%d", s->slice_res_idx);
    if (!strcmp(key, "fx_density"))
        return snprintf(buf, (size_t)buf_len, "%.0f", s->fx_density * 100.0f);
    if (!strcmp(key, "order_density"))
        return snprintf(buf, (size_t)buf_len, "%.0f", s->order_density * 100.0f);
    if (!strcmp(key, "pitch_range"))
        return snprintf(buf, (size_t)buf_len, "%d", s->pitch_range);
    if (!strcmp(key, "wet"))
        return snprintf(buf, (size_t)buf_len, "%.0f", s->wet * 100.0f);
    if (!strcmp(key, "ab"))
        return snprintf(buf, (size_t)buf_len, "%d", s->ab);
    if (!strcmp(key, "quantize"))
        return snprintf(buf, (size_t)buf_len, "%d", s->quantize_mode);
    /* trigger params always read back as 0 so autosave never re-fires them */
    if (!strcmp(key, "capture") || !strcmp(key, "arm") ||
        !strcmp(key, "reroll") || !strcmp(key, "clear"))
        return snprintf(buf, (size_t)buf_len, "0");
    if (!strcmp(key, "transport"))
        return snprintf(buf, (size_t)buf_len, "%d", s->follow_transport ? 0 : 1);
    if (!strcmp(key, "state")) {
        /* full settings snapshot — powers slot autosave AND module presets
         * (save/recall from the shadow UI). Audio is never serialized. */
        int n = snprintf(buf, (size_t)buf_len,
            "{\"loop_len\":%d,\"slice_res\":%d,\"fx_density\":%d,"
            "\"order_density\":%d,\"pitch_range\":%d,\"wet\":%d,\"ab\":%d,"
            "\"quantize\":%d,\"seed\":%u,\"transport\":%d,\"locks\":\"",
            s->loop_len_idx, s->slice_res_idx, (int)(s->fx_density * 100.0f),
            (int)(s->order_density * 100.0f), s->pitch_range,
            (int)(s->wet * 100.0f), s->ab, s->quantize_mode, s->seed,
            s->follow_transport ? 0 : 1);
        if (n < 0 || n >= buf_len - 3) return -1;
        int first = 1;
        for (int i = 0; i < SMACK_MAX_SLICES && n < buf_len - 12; i++) {
            if (!s->fx_locked[i]) continue;
            n += snprintf(buf + n, (size_t)(buf_len - n), "%s%d:%d",
                          first ? "" : ",", i, s->fx[i]);
            first = 0;
        }
        n += snprintf(buf + n, (size_t)(buf_len - n), "\"}");
        return n;
    }
    if (!strcmp(key, "n_slices"))
        return snprintf(buf, (size_t)buf_len, "%d", s->n_slices);
    if (!strcmp(key, "play_slice")) { /* current output slice, -1 if idle */
        int ps = -1;
        if (s->state == SMACK_LOOPING && s->slice_frames > 0.0) {
            ps = (int)(s->play_pos / s->slice_frames);
            if (ps >= s->n_slices) ps = s->n_slices - 1;
        }
        return snprintf(buf, (size_t)buf_len, "%d", ps);
    }
    if (!strcmp(key, "pattern")) {
        /* fx codes per slice, for future step-LED UI: e.g. "0300102..." */
        int n = s->n_slices < buf_len - 1 ? s->n_slices : buf_len - 1;
        for (int i = 0; i < n; i++) buf[i] = (char)('0' + s->fx[i]);
        buf[n] = 0;
        return n;
    }
    return -1;
}
