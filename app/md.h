/* MODELAR: what the engine (engine.c, at block rate in the audio interrupt) and the panel (ui.c, the main loop)
 * share: the patch, the patterns, the system settings and the commands from the panel to the engine. */
#pragma once
#include <stdint.h>
#include "synth.h"

#define NWHITE 16
#define NBLACK 11
#define NPAT 8                           /* patterns a patch */
#define NSTEP 64
#define NLANE 5                          /* KAOS, OSC1..4 */
#define NSLOT 16                         /* patches in the flash (and one more: the quick backup) */
#define NAME_LEN 8

/* ---- modulation destinations: each a value 0..1 (synth.h's parameters are made from them) */
enum {
    D_NONE, D_CUTOFF, D_RESO, D_MORPH, D_XMOD, D_PITCH, D_FX, D_DRIVE, D_FOLD, D_RING, D_CRUSH, D_CHORUS,
    D_DELAY, D_DFDBK, D_REVERB, D_VCA, D_VOL1, D_VOL2, D_VOL3, D_VOL4, D_PIT1, D_PIT2, D_PIT3, D_PIT4,
    D_LFO1, D_LFO2, D_CLOCK, D_CURX, D_CURY, ND
};
#define ND_PAD D_CURX                    /* the pad's and the keys' destinations stop before the cursor's own */

/* ---- FX modules (the FX page's rows), each ON, its intensity, A, B (0..100 unless said) */
enum { FX_VCF, FX_VCA, FX_DRIVE, FX_FOLD, FX_RING, FX_CHORUS, FX_DELAY, FX_REVERB, FX_CRUSH, NFX };
typedef struct {
    uint8_t on, amt, a, b;
} fxp_t;

/* ---- gestures (the black keys) */
enum { G_CIRCLE, G_EIGHT, G_SWEEPH, G_SWEEPV, G_DIAG, G_SPIRAL, G_TREMOLO, G_WALK, G_BOUNCE, G_JUMP, G_RETURN,
       NGEST };

/* ---- clock divisions: steps a quarter note (the clock's pulse) */
#define NRATE 12
extern const float RATE_SPQ[NRATE];
extern const char *const RATE_NAME[NRATE];
#define NGSPEED 11                       /* gesture speeds: 5 in bars, 6 in Hz */

typedef struct {                         /* a key's own settings (GLO) */
    uint8_t x, y;                        /* white: its point, 0..100; black: its gesture's centre (255: around the
                                          * point held or the cursor) */
    uint8_t gest;                        /* black: its gesture (G_*) */
    uint8_t speed, size;                 /* black: 0 = EDIT's, else speed index + 1 / size 1..100 */
    uint8_t intens;                      /* 0..100 */
    uint8_t mask;                        /* oscillators it moves (bit 0 = OSC1) */
    uint8_t xdest, ydest;                /* 0: EDIT's; else its own (D_*) in place of EDIT's */
    int8_t xamt, yamt;
    uint8_t snap;                        /* white: a snapshot taken (recalled at the touch) */
    uint8_t snap_vol[SYN_NOSC], snap_algo;
} keyp_t;

typedef struct {
    uint8_t shape, rate, sync, dest;     /* rate: free 0..99 (0.02 .. 20 Hz) or a division (RATE_*) */
    int8_t amt;
    uint8_t rsv[3];
} lfop_t;

#define PATCH_MAGIC 0x4C444F4Du          /* "MODL" */
#define PATCH_FORMAT 2u
typedef struct {
    uint32_t magic, format;
    char name[NAME_LEN];
    int8_t wave[SYN_NOSC], semi[SYN_NOSC], fine[SYN_NOSC];
    uint8_t algo, sel, fx_level, rsv0;  /* fx_level: the FX' dry/wet, 0..100 (50: as set) */
    fxp_t fx[NFX];
    /* EDIT */
    uint8_t xdest[2], ydest[2];
    int8_t xamt[2], yamt[2];
    int16_t slew_ms, attack_ms, release_ms;
    uint8_t rel_mode, range, curve, gsize, gspeed, rsv1[3];
    /* LFO, CLOCK */
    lfop_t lfo[2];
    uint16_t clock_chz;                  /* the clock's rate, centi-Hz (5 .. 20000) */
    uint8_t clock_src, clock_swing, clock_jitter, rsv2[3];
    /* ENV */
    uint8_t env, drift, drift_speed, mutation, density, rsv3[3];
    /* ARP */
    uint8_t arp_on, arp_mode, arp_rate, arp_gate, arp_glide, arp_swing, arp_latch, arp_spread;
    /* SEQ */
    uint8_t pat, seq_rate, seq_swing, seq_dir, rsv4[4];
    keyp_t key[NKEYS];
    uint8_t rsv[32];
} patch_t;

typedef struct {                         /* a KAOS step: where, touched or not, a gesture, glide; 3 more points
                                          * inside the step (live recording) */
    uint8_t x, y, f, g;                  /* f: bit 0 touch, bit 1 glide, bit 2 has sub-points; g: gesture + 1 */
    uint8_t sx[3], sy[3];
} kstep_t;
#define KS_TOUCH 1u
#define KS_GLIDE 2u
#define KS_SUB 4u
typedef struct {                         /* an OSC step: its tone, on, muted */
    int8_t semi;
    uint8_t f;                           /* bit 0 on, bit 1 mute */
} ostep_t;
#define OS_ON 1u
#define OS_MUTE 2u
typedef struct {
    uint8_t len[NLANE], mut[NLANE];
    uint8_t rsv[6];
    kstep_t k[NSTEP];
    ostep_t o[SYN_NOSC][NSTEP];
} pattern_t;

#define SYS_MAGIC 0x5359534Du            /* "MSYS" */
typedef struct {
    uint32_t magic;
    uint8_t slot;                        /* the patch in use */
    uint8_t midi_ch;                     /* 0 = every channel, 1..16 */
    uint8_t midi_notes, cc_out, theme, leds, rsv[10];
} sys_t;

extern patch_t patch;
extern pattern_t pats[NPAT];
extern sys_t sys;
extern int32_t vol[SYN_NOSC];            /* the knobs: never saved, 0 at power-on and at every patch */

/* ---- the engine's state, for the screen and the lights (written by the engine only) */
typedef struct {
    int32_t cx, cy, amt;                 /* the cursor and the pad's effect, 0..1000 */
    int32_t hold;                        /* HOLD on: the white key held (index 0..15), else -1 */
    uint32_t gest_on;                    /* gestures playing: bit per black key (latched or held) */
    uint32_t latched;                    /* gestures latched */
    int32_t playing, rec;
    int32_t pos[NLANE];                  /* each lane's step */
    int32_t arp_pt;                      /* the white key ARP touches now, -1 none */
    float clock_hz;                      /* the clock now (with its modulation) */
    uint32_t pulses;                     /* clock pulses (quarters) since the start */
    float lfo[2];                        /* -1..1 */
    float p[ND];                         /* the destinations now, 0..1 */
    int32_t cutoff_hz;
    int32_t midi_clock;                  /* MIDI clock coming in */
} eng_out_t;
extern volatile eng_out_t eng;

/* ---- the panel to the engine: commands, and the keys that play the pad */
enum { C_NONE, C_HOLD, C_LATCH, C_STOP_GEST, C_PLAY, C_STOP, C_REC, C_TAP, C_STEP_KEY, C_STEP_GEST, C_RESEED };
void eng_cmd(int c, int a);
extern volatile uint32_t pad_keys;       /* keys held that play the pad (0 on the SEQ and GLO pages) */
extern volatile int32_t eng_lane;        /* the lane REC writes and step entry fills */
extern volatile int32_t fx_bypass;       /* SHIFT + FX: every FX out (the VCF stays) */

void eng_init(void);
void eng_block(uint32_t n);              /* the audio interrupt, before syn_render */
void env_apply(int e);                   /* an environment's character into the patch */
void env_edit_defaults(int e);           /* EDIT as the environment has it */
extern const char *const ENV_NAME[8];
extern const char *const GEST_NAME[NGEST];
extern const char *const DEST_NAME[ND];
void pattern_defaults(pattern_t *p);
void key_defaults(void);
void patch_defaults(void);
