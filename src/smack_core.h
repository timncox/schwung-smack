/*
 * Smack — quantized live-loop capture + probabilistic per-slice glitch FX.
 *
 * Shared engine used by both builds:
 *   - smack_fx.c   audio_fx_api_v2 wrapper (chain slots + Master FX slots)
 *   - smack_gen.c  plugin_api_v2 wrapper (sound_generator reading hardware input)
 *
 * Timing model: 4/4, 16th-note steps, MIDI clock at 24 ppqn (6 ticks/step,
 * 96 ticks/bar). Falls back to host get_bpm() free-run when no clock runs.
 * The render path is non-allocating and non-blocking per schwung's realtime
 * rules; all buffers are allocated in smack_create().
 */
#ifndef SMACK_CORE_H
#define SMACK_CORE_H

#include <stdint.h>
#include "plugin_api_v1.h"

#define SMACK_SR          44100
#define SMACK_MAX_SECONDS 70               /* 16 bars at 55 BPM */
#define SMACK_RING_FRAMES (SMACK_SR * SMACK_MAX_SECONDS)
#define SMACK_MAX_SLICES  512              /* 16 bars x 16 steps x 2 (half-step res) */
#define SMACK_EDGE_FADE   96               /* ~2.2 ms fade at slice/loop edges */

typedef enum { SMACK_IDLE = 0, SMACK_ARMED, SMACK_RECORDING, SMACK_LOOPING } smack_state_t;

typedef enum {
    SMACK_FX_NONE = 0,
    SMACK_FX_RETRIG,     /* repeat first 1/2..1/8 of slice, decaying */
    SMACK_FX_REVERSE,
    SMACK_FX_PITCH,      /* varispeed +/- semitones (fxp), wraps in slice */
    SMACK_FX_SPEED,      /* fxp 0 = half speed, 1 = double speed */
    SMACK_FX_GATE,       /* 8-segment tremolo chop */
    SMACK_FX_BUZZ,       /* tiny-window repeat frozen at slice head */
    SMACK_FX_CRUSH,      /* sample-hold rate reduce + bit quantize */
    /* Looperator-inspired additions */
    SMACK_FX_REPEAT,     /* play head of slice, then stutter it (fxp: 1/2,1/4,3/4) */
    SMACK_FX_REVAFTER,   /* forward, then backwards from split (fxp: 1/2,2/3,3/4) */
    SMACK_FX_TAPESTOP,   /* varispeed ramp to zero (fxp 0 full-slice, 1 fast) */
    SMACK_FX_TAPESTART,  /* turntable spin-up from zero */
    SMACK_FX_SCRATCH,    /* oscillating playhead, vinyl wobble (fxp cycles) */
    SMACK_FX_ENV,        /* volume shape: fade-in/out, wobble, half-gate */
    SMACK_FX_PAN,        /* hard L / hard R / ping-pong within slice */
    SMACK_FX_FILTER,     /* LP/HP sweep across the slice (fxp direction) */
    SMACK_FX_VOWEL,      /* morphing 3-formant vowel filter (fxp pair) */
    SMACK_FX_TONALDELAY, /* feedback delay with swept time = pitched repeats */
    SMACK_FX_FREEZE,     /* granular freeze on the slice head (fxp spray) */
    SMACK_FX_DELAY,      /* tempo-synced echo (fxp: 16th, 8th, dotted, pingpong) */
    SMACK_FX_DIST,       /* waveshaper dirt (fxp: soft, hard, fold, gnash) */
    SMACK_FX_PHASER,     /* 4-stage allpass sweep (fxp: up, down, wobble, fast) */
    SMACK_FX_VERB,       /* gated Schroeder reverb burst (fxp: size/decay) */
    SMACK_FX_COUNT
} smack_fx_t;

typedef struct smack smack_t;

smack_t *smack_create(const host_api_v1_t *host);
void     smack_destroy(smack_t *s);

/* Process one block, stereo interleaved int16. in and out may alias. */
void smack_process(smack_t *s, const int16_t *in, int16_t *out, int frames);

void smack_on_midi(smack_t *s, const uint8_t *msg, int len, int source);
void smack_set_param(smack_t *s, const char *key, const char *val);
int  smack_get_param(smack_t *s, const char *key, char *buf, int buf_len);

#endif /* SMACK_CORE_H */
