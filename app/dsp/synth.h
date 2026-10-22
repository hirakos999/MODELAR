/* MODELAR: the sound. Four oscillators always sounding (a drone), routed into each other by an algorithm
 * (ALGO), then the FX chain, in stereo from the chorus on:
 *
 *   OSC1..4 -> ROUTING -> VCF -> DRIVE -> FOLD -> RING -> CRUSH -> VCA -> CHORUS -> DELAY -> REVERB -> soft clip
 *
 * syn_render() runs in the audio interrupt; its parameters (syn_in) are set by the engine (engine.c) at the start
 * of each block, in the same interrupt, and glided across the block here where a step would click.
 * Float only, no libm: fastmath.h. */
#pragma once
#include <stdint.h>

#define SYN_SR 44100.0f
#define SYN_NOSC 4
#define SYN_BASE_NOTE 48                 /* C3: every oscillator's tone is relative to it */

enum { W_SINE, W_TRI, W_SAW, W_SQUARE, W_PULSE, W_SSAW, W_NOISE, W_SH, W_MORPH, W_FM2, W_FORMANT, W_CHAOS,
       SYN_NWAVES };
enum { A_MIX, A_PAIRS, A_CHAIN, A_RING, A_SYNC, A_CROSS, A_AM, A_FEEDBACK, SYN_NALGOS };
enum { VCF_LP, VCF_BP, VCF_HP };

extern const char *const SYN_WAVE_NAME[SYN_NWAVES];
extern const char *const SYN_WAVE_SHORT[SYN_NWAVES];
extern const char *const SYN_ALGO_NAME[SYN_NALGOS];

typedef struct {
    int32_t wave[SYN_NOSC];              /* W_* */
    float note[SYN_NOSC];                /* MIDI note, fractional */
    float gain[SYN_NOSC];                /* 0..1 (the knob's curve already applied) */
    float xm[SYN_NOSC];                  /* how hard each is modulated by the routing, 0..1 */
    int32_t algo;                        /* A_* */
    float morph;                         /* 0..1: the morphing waves, the pulse's width */
    int32_t vcf_on, vcf_mode;
    float cutoff, reso;                  /* Hz; 0..1 */
    float drive, drive_tone;             /* 0..1 each: drive 0 = out of the chain */
    float fold, fold_sym;
    float ring, ring_hz;
    float crush_bits, crush_rate;        /* 0..1 each: 0 = untouched */
    float vca;                           /* the level, 0..1 */
    float chorus, chorus_rate, chorus_depth;   /* mix 0..1, Hz, 0..1 */
    float delay, delay_s, delay_fb;      /* mix 0..1, seconds, 0..1 */
    float reverb, rev_size, rev_damp;    /* mix 0..1, 0..1, 0..1 */
} syn_in_t;

typedef struct {
    int32_t level[SYN_NOSC];             /* each oscillator's output level, 0..1000 (the bars breathe with it) */
    int32_t out_level;                   /* the output's, 0..1000 */
} syn_out_t;

extern syn_in_t syn_in;
extern volatile syn_out_t syn_out;

void syn_init(void);
void syn_render(int32_t *lr, uint32_t n, uint32_t master_q12);   /* audio interrupt: n stereo frames, 24-bit */
void syn_panic(void);                                            /* silence now, for good (an update follows) */
float syn_wave_preview(int32_t wave, float ph, float morph);     /* one cycle for the screen: ph 0..1, -1..1 */
float syn_osc_hz(int i);                                         /* an oscillator's frequency now */
