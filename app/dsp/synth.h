/* MODELAR: the instrument. Four oscillators always sounding (a drone), routed into each other by an
 * algorithm (ALGO), then a low-pass filter. A cursor on an XY pad (the KAOS PAD, played on the white
 * keys) modulates them: X the filter's cutoff and the morphing waves, Y how hard the oscillators
 * modulate each other. The pad's effect fades in while a key is touched and out when it is let go.
 *
 *   OSC1..4 -> ROUTING (ALGO) -> VCF -> soft clip -> MASTER
 *
 * Threads: syn_render() runs in the audio interrupt and reads syn_in once a block; the UI writes
 * syn_in (whole words, so a block sees each value old or new, never torn) and reads syn_out.
 * Float only, no libm: fastmath.h. */
#pragma once
#include <stdint.h>

#define SYN_SR 44100.0f
#define SYN_NOSC 4
#define SYN_BASE_NOTE 48                 /* C3: every oscillator's tone is relative to it */

enum { W_SINE, W_TRI, W_SAW, W_SQUARE, W_PULSE, W_SSAW, W_NOISE, W_SH, W_MORPH, W_FM2, W_FORMANT, W_CHAOS,
       SYN_NWAVES };
enum { A_MIX, A_PAIRS, A_CHAIN, A_RING, A_SYNC, A_CROSS, A_AM, A_FEEDBACK, SYN_NALGOS };

extern const char *const SYN_WAVE_NAME[SYN_NWAVES];
extern const char *const SYN_WAVE_SHORT[SYN_NWAVES];
extern const char *const SYN_ALGO_NAME[SYN_NALGOS];

typedef struct {
    int32_t wave[SYN_NOSC];              /* W_* */
    int32_t semi[SYN_NOSC];              /* semitones from SYN_BASE_NOTE, -48..+48 */
    int32_t fine[SYN_NOSC];              /* cents, -50..+50 */
    int32_t vol[SYN_NOSC];               /* 0..100 */
    int32_t algo;                        /* A_* */
    int32_t touch;                       /* keys held on the pad: 0 = let go; more keys, more intense */
    int32_t tx, ty;                      /* the cursor's target, 0..1000 (it glides there: slew_ms) */
    int32_t slew_ms;                     /* the cursor's glide, 0..2000 */
    int32_t attack_ms, release_ms;       /* the pad's effect fading in on a touch, out on letting go */
    int32_t range;                       /* 0..100: the most the pad can modulate ("not too aggressive") */
} syn_in_t;

typedef struct {
    int32_t cx, cy;                      /* the cursor where it is now, 0..1000 */
    int32_t amt;                         /* the pad's effect now, 0..1000 */
    int32_t level[SYN_NOSC];             /* each oscillator's output level, 0..1000 (the bars breathe with it) */
    int32_t cutoff_hz;                   /* the filter now */
    int32_t xmod;                        /* the cross-modulation now, 0..1000 */
} syn_out_t;

extern volatile syn_in_t syn_in;
extern volatile syn_out_t syn_out;

void syn_init(void);
void syn_render(int32_t *lr, uint32_t n, uint32_t master_q12);   /* audio interrupt: n stereo frames, 24-bit */
void syn_panic(void);                                            /* silence now: volumes to 0 at once */
float syn_wave_preview(int32_t wave, float ph, float morph);     /* one cycle for the screen: ph 0..1, -1..1 */
