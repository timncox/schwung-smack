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
    return s;
}

void smack_destroy(smack_t *s) {
    if (!s) return;
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
        break;
    case 0xFC: s->clock_running = 0; break;
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
    float g = 1.0f;
    double src;

    if (!s->ab) {
        src = s->play_pos;                      /* A: clean, original order */
    } else {
        int sslice = s->order[oslice];
        int f  = s->fx[oslice];
        int fp = s->fxp[oslice];
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
        default: break;
        }
        if (rp < 0.0) rp = 0.0;
        if (rp > sf - 1.0) rp = sf - 1.0;
        src = (double)sslice * sf + rp;

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
    *l *= g;
    *r *= g;

    if (s->fx[oslice] == SMACK_FX_CRUSH && s->ab) {
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

        if (s->state != SMACK_LOOPING) {
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
        /* trigger params fire only on non-zero: UIs and autosave restores
         * send "0" on init, which must be a no-op */
        if (atoi(val)) {
            s->seed = s->seed * 1664525u + 1013904223u;
            if (s->state == SMACK_LOOPING) roll_pattern(s);
        }
    } else if (!strcmp(key, "capture")) {
        if (atoi(val)) capture_retro(s);
    } else if (!strcmp(key, "arm")) {
        if (atoi(val) && (s->state == SMACK_IDLE || s->state == SMACK_LOOPING)) {
            s->state = SMACK_ARMED;
            s->arm_start_flag = !s->clock_running; /* free-run: start next block */
        }
    } else if (!strcmp(key, "clear")) {
        if (atoi(val)) {
            s->state = SMACK_IDLE;
            s->ab_pending = -1;
            memset(s->fx_locked, 0, sizeof(s->fx_locked));
        }
    } else if (!strncmp(key, "lock_slice_", 11)) {
        int i = clampi(atoi(key + 11), 0, SMACK_MAX_SLICES - 1);
        int f = clampi(atoi(val), 0, SMACK_FX_COUNT - 1);
        s->fx[i] = (uint8_t)f;
        s->fx_locked[i] = 1;
    }
}

int smack_get_param(smack_t *s, const char *key, char *buf, int buf_len) {
    if (!s || !key || !buf || buf_len < 2) return -1;
    if (!strcmp(key, "state"))
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
    if (!strcmp(key, "n_slices"))
        return snprintf(buf, (size_t)buf_len, "%d", s->n_slices);
    if (!strcmp(key, "pattern")) {
        /* fx codes per slice, for future step-LED UI: e.g. "0300102..." */
        int n = s->n_slices < buf_len - 1 ? s->n_slices : buf_len - 1;
        for (int i = 0; i < n; i++) buf[i] = (char)('0' + s->fx[i]);
        buf[n] = 0;
        return n;
    }
    return -1;
}
