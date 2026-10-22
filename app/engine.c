/* MODELAR: the engine, at block rate in the audio interrupt (app_render, before syn_render). It turns the patch,
 * the keys that play the pad, the clock and its followers (LFOs, gestures, ARP, SEQ), the environment's drift and
 * MIDI into the sound's parameters (synth.h syn_in). Part of the unity build (app.c).
 *
 * Every destination (md.h D_*) is a value 0..1: its base comes from the patch (an FX module's intensity, a knob);
 * the LFOs and the drift add to it; then the pad, like a Kaoss Pad, pulls it towards the cursor's place, as far as
 * the pad's effect (it fades in on a touch, out on letting go) times the destination's amount times RANGE. */

const float RATE_SPQ[NRATE] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 1.5f, 3.0f, 6.0f, 0.6666667f, 1.3333333f,
                               2.6666667f};
const char *const RATE_NAME[NRATE] = {"1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/4T", "1/8T", "1/16T", "1/4.",
                                      "1/8.", "1/16."};
const char *const ENV_NAME[8] = {"CALMA", "NEBBIA", "MAREA", "FORESTA", "VETRO", "RUGGINE", "COSMO", "TEMPESTA"};
const char *const GEST_NAME[NGEST] = {"CIRCLE", "EIGHT", "SWEEP H", "SWEEP V", "DIAGONAL", "SPIRAL", "TREMOLO",
                                      "WALK", "BOUNCE", "JUMP", "RETURN"};
const char *const DEST_NAME[ND] = {"---", "CUTOFF", "RESO", "MORPH", "XMOD", "PITCH", "FX AMT", "DRIVE", "FOLD",
                                   "RING", "CRUSH", "CHORUS", "DELAY", "DLY FDBK", "REVERB", "VCA", "VOL 1",
                                   "VOL 2", "VOL 3", "VOL 4", "PITCH 1", "PITCH 2", "PITCH 3", "PITCH 4",
                                   "LFO1 RATE", "LFO2 RATE", "CLOCK", "CURSOR X", "CURSOR Y"};
static const float GSPEED_BARS[5] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
static const float GSPEED_HZ[6] = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
static const uint8_t WKEY[NWHITE] = PLAT_WHITE_KEYS, BKEY[NBLACK] = PLAT_BLACK_KEYS;

patch_t patch;
pattern_t pats[NPAT];
sys_t sys;
int32_t vol[SYN_NOSC];
volatile eng_out_t eng;
volatile uint32_t pad_keys;
volatile int32_t eng_lane;
volatile int32_t fx_bypass;

/* ------------------------------------------------------------------ commands --- */
static volatile uint32_t cq[32], cq_w, cq_r;
void eng_cmd(int c, int a)
{
    uint32_t w = cq_w;
    if (w - cq_r >= 32u)
        return;                          /* full: (never, at the panel's pace) */
    cq[w & 31u] = (uint32_t)c | (uint32_t)(a & 0xFFFF) << 8;
    cq_w = w + 1u;
}

/* ------------------------------------------------------------------- helpers --- */
static uint32_t rng_s = 0x2545F491u;
static uint32_t rnd(void)
{
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 17;
    rng_s ^= rng_s << 5;
    return rng_s;
}
static float frnd(void) { return (float)(rnd() >> 8) * (1.0f / 16777216.0f); }   /* 0..1 */
static float fclamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
static float slew_c(float dt, float ms) { return ms <= 1.0f ? 1.0f : 1.0f - fm_expf(-dt * 1000.0f / ms); }
static float tri01(float ph) { return 1.0f - fm_fabsf(2.0f * (ph - fm_floorf(ph)) - 1.0f); }   /* 0..1..0 */

/* ------------------------------------------------------------------ defaults --- */
void key_defaults(void)
{
    int i;
    for (i = 0; i < NKEYS; i++) {
        keyp_t *k = &patch.key[i];
        k->x = k->y = 255;
        k->gest = 0;
        k->speed = k->size = 0;
        k->intens = 100;
        k->mask = 0xF;
        k->xdest = k->ydest = 0;
        k->xamt = k->yamt = 0;
        k->snap = 0;
    }
    for (i = 0; i < NWHITE; i++) {
        patch.key[WKEY[i]].x = (uint8_t)((i % 4) * 100 / 3);
        patch.key[WKEY[i]].y = (uint8_t)((i / 4) * 100 / 3);
    }
    for (i = 0; i < NBLACK; i++)
        patch.key[BKEY[i]].gest = (uint8_t)i;
}

void pattern_defaults(pattern_t *p)
{
    int i, j;
    for (i = 0; i < NLANE; i++) {
        p->len[i] = 16;
        p->mut[i] = 0;
    }
    for (i = 0; i < NSTEP; i++) {
        p->k[i].x = p->k[i].y = 50;
        p->k[i].f = 0;
        p->k[i].g = 0;
        for (j = 0; j < SYN_NOSC; j++) {
            p->o[j][i].semi = 0;
            p->o[j][i].f = 0;
        }
    }
}

/* the environments: drift, its speed, mutation, density; the pad's X and Y; the FX' colour */
static const struct {
    uint8_t drift, dspeed, mut, dens, xd, yd, cut, reso, chorus, delay, reverb, drive, fold, crush;
} ENVS[8] = {
    /*              drift spd mut den  X        Y          cut res cho dly rev drv fold crush */
    /* CALMA    */ {8, 10, 0, 0, D_CUTOFF, D_REVERB, 70, 15, 0, 0, 55, 0, 0, 0},
    /* NEBBIA   */ {30, 15, 5, 5, D_CUTOFF, D_CHORUS, 45, 20, 50, 0, 40, 0, 0, 0},
    /* MAREA    */ {45, 6, 5, 0, D_CUTOFF, D_XMOD, 60, 25, 20, 20, 35, 0, 0, 0},
    /* FORESTA  */ {20, 40, 10, 45, D_CUTOFF, D_DELAY, 65, 30, 0, 40, 30, 0, 0, 0},
    /* VETRO    */ {15, 25, 10, 15, D_MORPH, D_XMOD, 90, 35, 25, 25, 45, 0, 0, 0},
    /* RUGGINE  */ {25, 30, 15, 10, D_CUTOFF, D_FOLD, 55, 45, 0, 15, 20, 45, 20, 20},
    /* COSMO    */ {35, 5, 10, 5, D_XMOD, D_REVERB, 60, 25, 30, 35, 75, 0, 0, 0},
    /* TEMPESTA */ {60, 70, 40, 40, D_XMOD, D_DRIVE, 70, 55, 10, 30, 30, 30, 15, 0},
};

void env_edit_defaults(int e)
{
    e &= 7;
    patch.xdest[0] = ENVS[e].xd;
    patch.xamt[0] = 100;
    patch.ydest[0] = ENVS[e].yd;
    patch.yamt[0] = 100;
    patch.xdest[1] = patch.ydest[1] = D_NONE;
    patch.slew_ms = 300;
    patch.attack_ms = 80;
    patch.release_ms = 600;
    patch.rel_mode = 0;
    patch.range = 85;
    patch.curve = 1;
    patch.gsize = 40;
    patch.gspeed = 2;
}

void env_apply(int e)
{
    e &= 7;
    patch.env = (uint8_t)e;
    patch.drift = ENVS[e].drift;
    patch.drift_speed = ENVS[e].dspeed;
    patch.mutation = ENVS[e].mut;
    patch.density = ENVS[e].dens;
    patch.xdest[0] = ENVS[e].xd;
    patch.xamt[0] = 100;
    patch.ydest[0] = ENVS[e].yd;
    patch.yamt[0] = 100;
    patch.xdest[1] = patch.ydest[1] = D_NONE;
    patch.fx[FX_VCF].on = 1;
    patch.fx[FX_VCF].amt = ENVS[e].cut;
    patch.fx[FX_VCF].a = ENVS[e].reso;
    patch.fx[FX_CHORUS].on = ENVS[e].chorus > 0;
    patch.fx[FX_CHORUS].amt = ENVS[e].chorus;
    patch.fx[FX_DELAY].on = ENVS[e].delay > 0;
    patch.fx[FX_DELAY].amt = ENVS[e].delay;
    patch.fx[FX_REVERB].on = ENVS[e].reverb > 0;
    patch.fx[FX_REVERB].amt = ENVS[e].reverb;
    patch.fx[FX_DRIVE].on = ENVS[e].drive > 0;
    patch.fx[FX_DRIVE].amt = ENVS[e].drive;
    patch.fx[FX_FOLD].on = ENVS[e].fold > 0;
    patch.fx[FX_FOLD].amt = ENVS[e].fold;
    patch.fx[FX_CRUSH].on = ENVS[e].crush > 0;
    patch.fx[FX_CRUSH].amt = ENVS[e].crush;
}

void patch_defaults(void)
{
    static const int8_t W[SYN_NOSC] = {W_SAW, W_SINE, W_SQUARE, W_NOISE}, S[SYN_NOSC] = {0, 7, -12, 3};
    static const fxp_t FX[NFX] = {
        {1, 100, 20, VCF_LP}, {0, 50, 2, 0}, {0, 30, 50, 0}, {0, 30, 50, 0}, {0, 40, 40, 0},
        {0, 40, 30, 50}, {0, 30, 3, 40}, {1, 30, 60, 40}, {0, 30, 0, 0}};
    int i;
    for (i = 0; i < (int)sizeof patch; i++)
        ((uint8_t *)&patch)[i] = 0;
    patch.magic = PATCH_MAGIC;
    patch.format = PATCH_FORMAT;
    for (i = 0; i < NAME_LEN; i++)
        patch.name[i] = "INIT    "[i];
    for (i = 0; i < SYN_NOSC; i++) {
        patch.wave[i] = W[i];
        patch.semi[i] = S[i];
    }
    patch.algo = A_CROSS;
    patch.fx_level = 50;
    for (i = 0; i < NFX; i++)
        patch.fx[i] = FX[i];
    patch.xdest[0] = D_CUTOFF;
    patch.xamt[0] = 100;
    patch.xdest[1] = D_MORPH;
    patch.xamt[1] = 100;
    patch.ydest[0] = D_XMOD;
    patch.yamt[0] = 100;
    patch.ydest[1] = D_FX;
    patch.yamt[1] = 60;
    patch.slew_ms = 300;
    patch.attack_ms = 80;
    patch.release_ms = 600;
    patch.rel_mode = 0;                  /* FADE */
    patch.range = 85;
    patch.curve = 1;                     /* SOFT */
    patch.gsize = 40;
    patch.gspeed = 2;                    /* 1 bar */
    patch.lfo[0].shape = 0;
    patch.lfo[0].rate = 40;
    patch.lfo[0].dest = D_NONE;
    patch.lfo[0].amt = 30;
    patch.lfo[1].shape = 5;
    patch.lfo[1].rate = 3;               /* 1/8 */
    patch.lfo[1].sync = 1;
    patch.lfo[1].dest = D_NONE;
    patch.lfo[1].amt = 30;
    patch.clock_chz = 200;               /* 2.00 Hz = 120 BPM */
    patch.env = 1;
    patch.drift = 0;
    patch.drift_speed = 15;
    patch.arp_mode = 0;
    patch.arp_rate = 3;                  /* 1/8 */
    patch.arp_gate = 70;
    patch.arp_glide = 30;
    patch.arp_spread = 50;
    patch.seq_rate = 4;                  /* 1/16 */
    key_defaults();
    for (i = 0; i < NPAT; i++)
        pattern_defaults(&pats[i]);
}

/* ------------------------------------------------------------------- state --- */
static float t_now;                      /* seconds since the start */
static float clk_ph, tap_t[4];
static int tap_n;
static float midi_hz, midi_last24, midi_seen;
static uint32_t midi_ticks;
static float lfo_ph[2], lfo_sh[2][2];
static float drift_v[5], drift_t[5], drift_timer;
static float ev_x, ev_y, ev_until, ev_next;
static float cx = 0.5f, cy = 0.5f, amt;  /* the cursor (glided), the pad's effect */
static float bx = 0.5f, by = 0.5f;       /* the cursor's base (before gestures), glided */
static int32_t hold_key = -1;            /* the white key HOLD keeps (0..15) */
static uint32_t latched, prev_keys;
static int8_t order[NWHITE];             /* white keys in the order pressed */
static int norder;
typedef struct {
    float ph, ax, ay, wx, wy, vx, vy, jx, jy;
    int on;
} gest_t;
static gest_t gs[NBLACK + 1];            /* one a black key, and the SEQ's */
/* ARP */
static int8_t arp_set[NWHITE];
static int narp, arp_idx, arp_dir = 1, arp_fresh;
static float arp_ph, arp_len = 1.0f, arp_step_s;
static uint32_t arp_count;
/* SEQ */
static float seq_ph, seq_len = 1.0f;
static uint32_t seq_count;
static int8_t lane_dir[NLANE];
static int32_t rec_pos;
static float vca_ph;
static float pv[ND];                     /* the destinations as the last block left them */
static int32_t midi_cc_last[6];
static float cc_t;

static float clock_base_hz(void)
{
    if (patch.clock_src == 1 && t_now - midi_seen < 1.0f && midi_hz > 0.0f)
        return midi_hz;
    if (patch.clock_src == 2)
        return syn_osc_hz(3) / 64.0f;    /* OSC4: time and timbre tied */
    return (float)patch.clock_chz * 0.01f;
}

static void lane_step(pattern_t *pt, int l)   /* a lane to its next step, by the direction */
{
    int32_t len = pt->len[l] ? pt->len[l] : 16, p = eng.pos[l], wrapped = 0;
    switch (patch.seq_dir) {
    default:
        p = p + 1 >= len ? (wrapped = 1, 0) : p + 1;
        break;
    case 1:
        p = p <= 0 ? (wrapped = 1, len - 1) : p - 1;
        break;
    case 2:
        if (!lane_dir[l])
            lane_dir[l] = 1;
        p += lane_dir[l];
        if (p >= len || p < 0) {
            lane_dir[l] = (int8_t)-lane_dir[l];
            p += 2 * lane_dir[l];
            p = p < 0 ? 0 : p >= len ? len - 1 : p;
            wrapped = lane_dir[l] > 0;
        }
        break;
    case 3:
        p = (int32_t)(rnd() % (uint32_t)len);
        wrapped = p == 0;
        break;
    }
    eng.pos[l] = p;
    if (wrapped) {                       /* the lane came round: MUTATION changes a step a little, by chance */
        uint32_t m = pt->mut[l] > patch.mutation ? pt->mut[l] : patch.mutation;
        if (m && rnd() % 100u < m) {
            int s = (int)(rnd() % (uint32_t)len);
            if (l == 0) {
                kstep_t *k = &pt->k[s];
                if (k->f & KS_TOUCH) {
                    k->x = (uint8_t)clamp((int32_t)k->x + (int32_t)(rnd() % 31u) - 15, 0, 100);
                    k->y = (uint8_t)clamp((int32_t)k->y + (int32_t)(rnd() % 31u) - 15, 0, 100);
                    k->f &= (uint8_t)~KS_SUB;
                } else if (rnd() % 4u == 0) {
                    k->f |= KS_TOUCH;
                    k->x = (uint8_t)(rnd() % 101u);
                    k->y = (uint8_t)(rnd() % 101u);
                }
            } else {
                static const int8_t IV[6] = {-12, -5, -2, 2, 5, 7};
                ostep_t *o = &pt->o[l - 1][s];
                if (o->f & OS_ON)
                    o->semi = (int8_t)clamp(o->semi + IV[rnd() % 6u], -24, 24);
                else if (rnd() % 3u == 0) {
                    o->f |= OS_ON;
                    o->semi = IV[rnd() % 6u];
                }
            }
        }
    }
}

static void play_start(void)
{
    int l;
    eng.playing = 1;
    seq_ph = 0.0f;
    seq_len = 1.0f;
    seq_count = 0;
    clk_ph = 0.0f;
    arp_ph = 0.0f;
    for (l = 0; l < NLANE; l++) {
        eng.pos[l] = patch.seq_dir == 1 ? pats[patch.pat & 7].len[l] - 1 : 0;
        lane_dir[l] = 1;
    }
}

static void cmd_run(void)
{
    while (cq_r != cq_w) {
        uint32_t c = cq[cq_r & 31u], a = c >> 8;
        cq_r++;
        switch (c & 0xFFu) {
        case C_HOLD:                     /* a white key held with SHIFT: the cursor stays there (again: lets go) */
            hold_key = hold_key == (int32_t)a ? -1 : (int32_t)a;
            break;
        case C_LATCH:                    /* a black key with SHIFT: its gesture goes on by itself (again: stops) */
            latched ^= 1u << a;
            if (latched >> a & 1u) {
                gs[a].on = 0;
            }
            break;
        case C_STOP_GEST:
            latched = 0;
            hold_key = -1;
            break;
        case C_PLAY:
            if (eng.playing)
                eng.playing = 0;
            else
                play_start();
            break;
        case C_STOP:
            eng.playing = 0;
            break;
        case C_REC:
            eng.rec = !eng.rec;
            rec_pos = 0;
            break;
        case C_TAP: {                    /* the clock from the taps' pace */
            if (tap_n && t_now - tap_t[(tap_n - 1) & 3] > 3.0f)
                tap_n = 0;
            tap_t[tap_n & 3] = t_now;
            tap_n++;
            if (tap_n >= 2) {
                int m = tap_n > 4 ? 4 : tap_n;
                float span = tap_t[(tap_n - 1) & 3] - tap_t[(tap_n - m) & 3];
                if (span > 0.0f)
                    patch.clock_chz = (uint16_t)clamp((int32_t)((float)(m - 1) / span * 100.0f + 0.5f), 5, 20000);
            }
            break;
        }
        case C_STEP_KEY:                 /* REC while stopped: a key fills the step and moves on */
        case C_STEP_GEST: {
            pattern_t *pt = &pats[patch.pat & 7];
            int32_t lane = eng_lane, len = pt->len[lane] ? pt->len[lane] : 16;
            if (lane == 0) {
                kstep_t *k = &pt->k[rec_pos % len];
                if ((c & 0xFFu) == C_STEP_KEY) {
                    keyp_t *kp = &patch.key[WKEY[a & 15u]];
                    k->x = kp->x;
                    k->y = kp->y;
                    k->g = 0;
                } else {
                    k->g = (uint8_t)(patch.key[BKEY[a % NBLACK]].gest + 1u);
                }
                k->f = (uint8_t)((k->f | KS_TOUCH) & ~KS_SUB);
            } else if ((c & 0xFFu) == C_STEP_KEY) {
                static const int8_t SC[NWHITE] = {-12, -10, -8, -7, -5, -3, -1, 0, 2, 4, 5, 7, 9, 11, 12, 14};
                ostep_t *o = &pt->o[lane - 1][rec_pos % len];
                o->semi = SC[a & 15u];
                o->f = OS_ON;
            }
            rec_pos = (rec_pos + 1) % len;
            eng.pos[lane] = rec_pos;
            break;
        }
        case C_RESEED:
            rng_s ^= a * 2654435761u | 1u;
            break;
        }
    }
}

/* MIDI in: the clock, start and stop, CCs and (as SYSTEM says) notes */
static void midi_run(void)
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu, ch = st & 15u;
        if (st == 0xF8u) {
            midi_seen = t_now;
            if (++midi_ticks % 24u == 0) {
                float span = t_now - midi_last24;
                midi_last24 = t_now;
                if (span > 0.004f && span < 30.0f)
                    midi_hz = midi_hz > 0.0f ? midi_hz + (1.0f / span - midi_hz) * 0.5f : 1.0f / span;
            }
            continue;
        }
        if (st == 0xFAu || st == 0xFBu) {
            if (patch.clock_src == 1 && !eng.playing)
                play_start();
            midi_ticks = 0;
            continue;
        }
        if (st == 0xFCu) {
            if (patch.clock_src == 1)
                eng.playing = 0;
            continue;
        }
        if (sys.midi_ch && ch + 1u != sys.midi_ch)
            continue;
        if ((st & 0xF0u) == 0xB0u) {
            switch (d1) {
            case 12: ev_x = (float)d2 / 127.0f; ev_until = t_now + 0.4f; break;
            case 13: ev_y = (float)d2 / 127.0f; ev_until = t_now + 0.4f; break;
            case 14: case 15: case 16: case 17: vol[d1 - 14u] = (int32_t)(d2 * 100u / 127u); break;
            case 21: case 22: case 23: case 24:
                patch.wave[d1 - 21u] = (int8_t)(d2 * SYN_NWAVES / 128u);
                break;
            case 70: patch.algo = (uint8_t)(d2 * SYN_NALGOS / 128u); break;
            case 71: patch.fx[FX_VCF].a = (uint8_t)(d2 * 100u / 127u); break;
            case 74: patch.fx[FX_VCF].amt = (uint8_t)(d2 * 100u / 127u); break;
            case 91: patch.fx[FX_REVERB].amt = (uint8_t)(d2 * 100u / 127u); break;
            case 93: patch.fx[FX_CHORUS].amt = (uint8_t)(d2 * 100u / 127u); break;
            case 94: patch.fx[FX_DELAY].amt = (uint8_t)(d2 * 100u / 127u); break;
            default: break;
            }
        } else if ((st & 0xF0u) == 0x90u && d2 && sys.midi_notes) {
            patch.semi[patch.sel & 3] = (int8_t)clamp((int32_t)d1 - SYN_BASE_NOTE, -48, 48);
        }
    }
}

static float lfo_val(int i, float dt, float clock_hz, const float *p)
{
    const lfop_t *l = &patch.lfo[i];
    float hz, ph, v;
    if (l->shape == 6)
        hz = clock_hz;                   /* CLOCK: a square at the clock's pulse */
    else if (l->sync)
        hz = clock_hz * RATE_SPQ[l->rate % NRATE];
    else
        hz = 0.02f * fm_exp2f((float)(l->rate > 99 ? 99 : l->rate) * (10.0f / 99.0f));
    hz *= fm_exp2f((p[D_LFO1 + i] - 0.5f) * 6.0f);
    lfo_ph[i] += hz * dt;
    if (lfo_ph[i] >= 1.0f) {
        lfo_ph[i] -= fm_floorf(lfo_ph[i]);
        lfo_sh[i][0] = lfo_sh[i][1];
        lfo_sh[i][1] = frnd() * 2.0f - 1.0f;
    }
    ph = lfo_ph[i];
    switch (l->shape) {
    default:
    case 0: v = fm_sin_turns(ph); break;
    case 1: v = tri01(ph) * 2.0f - 1.0f; break;
    case 2: v = 1.0f - 2.0f * ph; break;
    case 3: case 6: v = ph < 0.5f ? 1.0f : -1.0f; break;
    case 4: v = lfo_sh[i][1]; break;
    case 5: v = lfo_sh[i][0] + (lfo_sh[i][1] - lfo_sh[i][0]) * (0.5f - 0.5f * fm_sin_turns(0.25f + 0.5f * ph)); break;
    }
    return v;
}

/* a gesture's offset from its anchor (ox, oy), or an absolute place (returns 1) */
static int gest_shape(gest_t *g, int type, float r, float dt, float hz, float *ox, float *oy)
{
    float a = g->ph * FM_TWO_PI;
    switch (type) {
    default:
    case G_CIRCLE: *ox = r * fm_cosf(a); *oy = r * fm_sinf(a); return 0;
    case G_EIGHT: *ox = r * fm_sinf(a); *oy = 0.5f * r * fm_sinf(2.0f * a); return 0;
    case G_SWEEPH: *ox = r * 1.6f * (tri01(g->ph) * 2.0f - 1.0f); *oy = 0.0f; return 0;
    case G_SWEEPV: *ox = 0.0f; *oy = r * 1.6f * (tri01(g->ph) * 2.0f - 1.0f); return 0;
    case G_DIAG: *ox = *oy = r * 1.2f * (tri01(g->ph) * 2.0f - 1.0f); return 0;
    case G_SPIRAL: {
        float rr = r * tri01(g->ph), b = a * 3.0f;
        *ox = rr * fm_cosf(b);
        *oy = rr * fm_sinf(b);
        return 0;
    }
    case G_TREMOLO:
        *ox = 0.18f * r * fm_sinf(a * 8.0f);
        *oy = 0.18f * r * fm_sinf(a * 8.0f * 1.31f + 1.0f);
        return 0;
    case G_WALK:                         /* a slow drift that keeps near its anchor */
        g->vx += (frnd() - 0.5f) * dt * 4.0f * hz - g->wx * dt * 0.5f;
        g->vy += (frnd() - 0.5f) * dt * 4.0f * hz - g->wy * dt * 0.5f;
        g->vx *= 0.98f;
        g->vy *= 0.98f;
        g->wx = fm_clampf(g->wx + g->vx * dt * 4.0f, -r, r);
        g->wy = fm_clampf(g->wy + g->vy * dt * 4.0f, -r, r);
        *ox = g->wx;
        *oy = g->wy;
        return 0;
    case G_BOUNCE:                       /* a ball off the pad's edges, anywhere on it */
        if (g->vx == 0.0f && g->vy == 0.0f) {
            g->vx = 0.37f;
            g->vy = 0.23f;
            g->wx = g->ax;
            g->wy = g->ay;
        }
        g->wx += g->vx * dt * hz * (0.5f + 2.0f * r);
        g->wy += g->vy * dt * hz * (0.5f + 2.0f * r);
        if (g->wx < 0.0f || g->wx > 1.0f) {
            g->vx = -g->vx;
            g->wx = fclamp01(g->wx);
        }
        if (g->wy < 0.0f || g->wy > 1.0f) {
            g->vy = -g->vy;
            g->wy = fclamp01(g->wy);
        }
        *ox = g->wx;
        *oy = g->wy;
        return 1;
    case G_JUMP:                         /* a soft leap to a new place near the anchor, a cycle each (eng_block) */
        g->jx += (g->wx - g->jx) * 0.15f;
        g->jy += (g->wy - g->jy) * 0.15f;
        *ox = g->jx;
        *oy = g->jy;
        return 0;
    case G_RETURN:
        *ox = 0.5f;
        *oy = 0.5f;
        return 1;
    }
}

static float key_x(int w) { return (float)patch.key[WKEY[w]].x * 0.01f; }
static float key_y(int w) { return (float)patch.key[WKEY[w]].y * 0.01f; }

void eng_init(void)
{
    int i;
    for (i = 0; i < NBLACK + 1; i++)
        gs[i].on = 0;
    hold_key = -1;
    eng.hold = -1;
    eng.arp_pt = -1;
    t_now = 0.0f;
}

void eng_block(uint32_t n)
{
    float dt = (float)n / SYN_SR, p[ND], pre_xmod, pre_pitch, hz, sc;
    float tx = 0.5f, ty = 0.5f, touch = 0.0f, slew_ms = (float)patch.slew_ms, intens = 1.0f, gx = 0.0f, gy = 0.0f;
    float lfo_touch = 0.0f, lfo_x = 0.0f, lfo_y = 0.0f, curve_x, curve_y;
    uint32_t keys = pad_keys, white = 0, down, mask = 0, i;
    int nw = 0, has_base = 0, seq_g = 0, live = 0;
    int32_t semi_seq[SYN_NOSC] = {0, 0, 0, 0}, mute_seq[SYN_NOSC] = {0, 0, 0, 0};
    uint8_t xd[2], yd[2];
    int8_t xa[2], ya[2];
    pattern_t *pt = &pats[patch.pat & 7];

    t_now += dt;
    cmd_run();
    midi_run();

    /* ---- the clock (its modulation from the last block) */
    hz = clock_base_hz() * fm_exp2f((pv[D_CLOCK] - 0.5f) * 4.0f);
    hz = fm_clampf(hz, 0.05f, 200.0f);
    eng.clock_hz = hz;
    eng.midi_clock = t_now - midi_seen < 1.0f;
    clk_ph += hz * dt;
    while (clk_ph >= 1.0f) {
        clk_ph -= 1.0f;
        eng.pulses++;
    }

    /* ---- the keys: white ones in the order pressed, the black ones' gestures */
    for (i = 0; i < NWHITE; i++)
        if (keys >> WKEY[i] & 1u) {
            white |= 1u << i;
            mask |= patch.key[WKEY[i]].mask;
        }
    down = white & ~prev_keys;
    if (white != prev_keys) {
        int j, k;
        for (j = k = 0; j < norder; j++)         /* let go: out of the order */
            if (white >> order[j] & 1u)
                order[k++] = order[j];
        norder = k;
        for (i = 0; i < NWHITE; i++)
            if (down >> i & 1u)
                order[norder++] = (int8_t)i;
    }
    if (down)                                    /* snapshots: a key that has one recalls it */
        for (i = 0; i < NWHITE; i++)
            if ((down >> i & 1u) && patch.key[WKEY[i]].snap) {
                int o;
                for (o = 0; o < SYN_NOSC; o++)
                    vol[o] = patch.key[WKEY[i]].snap_vol[o];
                patch.algo = patch.key[WKEY[i]].snap_algo;
            }
    prev_keys = white;
    for (i = 0; i < NWHITE; i++)
        if (white >> i & 1u) {
            tx = nw ? tx + key_x((int)i) : key_x((int)i);
            ty = nw ? ty + key_y((int)i) : key_y((int)i);
            intens = nw ? (intens > (float)patch.key[WKEY[i]].intens * 0.01f ? intens
                                                                                : (float)patch.key[WKEY[i]].intens * 0.01f)
                        : (float)patch.key[WKEY[i]].intens * 0.01f;
            nw++;
        }
    if (nw) {
        tx /= (float)nw;
        ty /= (float)nw;
    }

    /* ---- ARP: the held points (kept with LATCH) in turn */
    if (patch.arp_on && nw) {                   /* the set: what is held (with LATCH, a fresh press starts anew) */
        int j;
        if (arp_fresh || !patch.arp_latch)
            narp = 0;
        arp_fresh = 0;
        for (j = 0; j < norder; j++) {
            int k, have = 0;
            for (k = 0; k < narp; k++)
                have |= arp_set[k] == order[j];
            if (!have && narp < NWHITE)
                arp_set[narp++] = order[j];
        }
    } else if (!nw) {
        arp_fresh = 1;
    }
    if (patch.arp_on && narp && (nw || patch.arp_latch)) {
        float spq = RATE_SPQ[patch.arp_rate % NRATE], sw = (float)patch.arp_swing / 150.0f, cxp = 0.0f, cyp = 0.0f,
              spread = fm_exp2f(((float)patch.arp_spread - 50.0f) / 50.0f);
        int j;
        for (j = 0; j < narp; j++) {
            cxp += key_x(arp_set[j]);
            cyp += key_y(arp_set[j]);
        }
        cxp /= (float)narp;
        cyp /= (float)narp;
        arp_ph += hz * spq * dt;
        arp_step_s = 1.0f / (hz * spq);
        if (arp_ph >= arp_len) {
            arp_ph -= arp_len;
            arp_count++;
            arp_len = (arp_count & 1u) ? 1.0f - sw : 1.0f + sw;
            switch (patch.arp_mode) {
            default:
            case 0: case 1: case 4: arp_idx = (arp_idx + 1) % narp; break;
            case 2:
                if (narp > 1) {
                    arp_idx += arp_dir;
                    if (arp_idx >= narp || arp_idx < 0) {
                        arp_dir = -arp_dir;
                        arp_idx += 2 * arp_dir;
                    }
                } else {
                    arp_idx = 0;
                }
                break;
            case 3: arp_idx = (int)(rnd() % (uint32_t)narp); break;
            case 5: arp_idx = (arp_idx + 1) % narp; break;
            }
        }
        if (arp_idx >= narp)
            arp_idx = 0;
        if (patch.arp_mode == 5) {               /* ORBIT: around the points' centre */
            float rad = 0.0f, a = ((float)arp_idx + arp_ph / arp_len) / (float)narp;
            for (j = 0; j < narp; j++)
                rad += fm_fabsf(key_x(arp_set[j]) - cxp) + fm_fabsf(key_y(arp_set[j]) - cyp);
            rad = narp > 1 ? rad / (float)narp * spread : 0.2f * spread;
            tx = cxp + rad * fm_cosf(a * FM_TWO_PI);
            ty = cyp + rad * fm_sinf(a * FM_TWO_PI);
            touch = 0.85f;
            eng.arp_pt = -1;
        } else {
            int k, w, sorted[NWHITE], ns = 0;
            for (k = 0; k < NWHITE; k++)         /* UP / DOWN / PING-PONG go by the keys' order on the keyboard */
                for (w = 0; w < narp; w++)
                    if (arp_set[w] == k)
                        sorted[ns++] = k;
            w = patch.arp_mode == 4 || patch.arp_mode == 3 ? arp_set[arp_idx]
                : patch.arp_mode == 1                     ? sorted[ns - 1 - arp_idx % ns]
                                                          : sorted[arp_idx % ns];
            tx = cxp + (key_x(w) - cxp) * spread;
            ty = cyp + (key_y(w) - cyp) * spread;
            touch = arp_ph / arp_len < (float)patch.arp_gate * 0.01f ? 0.85f : 0.0f;
            eng.arp_pt = w;
        }
        slew_ms = (float)patch.arp_glide * 0.01f * arp_step_s * 1000.0f;
        has_base = 1;
        live = 1;
    } else {
        eng.arp_pt = -1;
        if (nw) {
            touch = fm_minf(1.0f, 0.7f + 0.15f * (float)(nw - 1)) * intens;
            has_base = 1;
            live = 1;
        }
    }
    if (hold_key >= 0) {                         /* HOLD: over the keys and the ARP */
        tx = key_x(hold_key);
        ty = key_y(hold_key);
        touch = 0.85f;
        has_base = 1;
        live = 1;
    }

    /* ---- SEQ: the steps, each lane at its own length */
    if (eng.playing) {
        float spq = RATE_SPQ[patch.seq_rate % NRATE], sw = (float)patch.seq_swing / 150.0f + 0.0f;
        int l;
        seq_ph += hz * spq * dt;
        if (seq_ph >= seq_len) {
            float jit = (float)patch.clock_jitter * 0.003f * (frnd() * 2.0f - 1.0f);
            seq_ph -= seq_len;
            seq_count++;
            sw += (float)patch.clock_swing / 150.0f;
            seq_len = ((seq_count & 1u) ? 1.0f - sw : 1.0f + sw) * (1.0f + jit);
            if (seq_len < 0.1f)
                seq_len = 0.1f;
            for (l = 0; l < NLANE; l++)
                lane_step(pt, l);
        }
        {
            kstep_t *k = &pt->k[eng.pos[0] % NSTEP];
            float f = fclamp01(seq_ph / seq_len);
            if ((k->f & KS_TOUCH) && !(eng.rec && live)) {
                float sx = (float)k->x * 0.01f, sy = (float)k->y * 0.01f;
                if (k->f & KS_SUB) {             /* recorded live: four places inside the step, joined */
                    float q = f * 4.0f, fr;
                    int a = (int)q, b = a + 1;
                    float ax = a ? (float)k->sx[a - 1] * 0.01f : sx, ay = a ? (float)k->sy[a - 1] * 0.01f : sy;
                    float bxp = b < 4 ? (float)k->sx[b - 1] * 0.01f : ax, byp = b < 4 ? (float)k->sy[b - 1] * 0.01f : ay;
                    fr = q - (float)a;
                    sx = ax + (bxp - ax) * fr;
                    sy = ay + (byp - ay) * fr;
                }
                if (!live) {
                    tx = sx;
                    ty = sy;
                    touch = 0.85f;
                    has_base = 1;
                    slew_ms = (k->f & KS_GLIDE) ? (float)patch.slew_ms : 15.0f;
                }
                seq_g = k->g;
            }
            if (eng.rec && live) {               /* REC while playing: the touch into the step, four places */
                int q = (int)(f * 4.0f);
                if (q > 3)
                    q = 3;
                if (q == 0) {
                    k->x = (uint8_t)(cx * 100.0f);
                    k->y = (uint8_t)(cy * 100.0f);
                } else {
                    k->sx[q - 1] = (uint8_t)(cx * 100.0f);
                    k->sy[q - 1] = (uint8_t)(cy * 100.0f);
                    k->f |= KS_SUB;
                }
                if (q == 0) {                    /* (a step begun anew: its later places follow) */
                    k->sx[0] = k->sx[1] = k->sx[2] = k->x;
                    k->sy[0] = k->sy[1] = k->sy[2] = k->y;
                }
                k->f |= KS_TOUCH;
            }
            for (l = 1; l < NLANE; l++) {
                ostep_t *o = &pt->o[l - 1][eng.pos[l] % NSTEP];
                if (o->f & OS_ON) {
                    semi_seq[l - 1] = o->semi;
                    mute_seq[l - 1] = (o->f & OS_MUTE) != 0;
                }
            }
        }
    } else if (!eng.rec) {
        int l;
        for (l = 0; l < NLANE; l++)
            if (eng.pos[l] >= (pt->len[l] ? pt->len[l] : 16))
                eng.pos[l] = 0;
    }

    /* ---- ENV: drift and its random events */
    {
        float dspeed = 0.03f * fm_exp2f((float)patch.drift_speed * 0.06f), d = (float)patch.drift * 0.01f;
        drift_timer -= dt;
        if (drift_timer <= 0.0f) {
            drift_timer = 1.0f / dspeed;
            for (i = 0; i < 5; i++)
                drift_t[i] = frnd() * 2.0f - 1.0f;
        }
        for (i = 0; i < 5; i++)
            drift_v[i] += (drift_t[i] - drift_v[i]) * slew_c(dt, 400.0f / dspeed);
        if (d > 0.0f) {
            if (!has_base) {
                tx = cx + drift_v[0] * 0.05f;
                ty = cy + drift_v[1] * 0.05f;
                slew_ms = 2000.0f;
            }
            tx += drift_v[0] * d * 0.15f;
            ty += drift_v[1] * d * 0.15f;
            if (touch < d * 0.4f)
                touch = d * 0.4f;
        }
        if (patch.density && t_now >= ev_next && !live) {   /* an event: a soft touch somewhere, for a while */
            ev_x = frnd();
            ev_y = frnd();
            ev_until = t_now + 0.4f + 2.0f * frnd();
            ev_next = ev_until + (0.5f + 4.0f * frnd()) * (100.0f / (float)patch.density);
        }
        if (t_now < ev_until && !live) {
            tx = ev_x;
            ty = ev_y;
            if (touch < 0.6f)
                touch = 0.6f;
            has_base = 1;
        }
    }

    /* ---- the LFOs (they may move the cursor too) */
    for (i = 0; i < 2; i++) {
        float v = lfo_val((int)i, dt, hz, pv);
        eng.lfo[i] = v;
        if (patch.lfo[i].dest == D_CURX || patch.lfo[i].dest == D_CURY) {
            float a = (float)patch.lfo[i].amt * 0.01f;
            if (patch.lfo[i].dest == D_CURX)
                lfo_x += v * a * 0.5f;
            else
                lfo_y += v * a * 0.5f;
            if (lfo_touch < fm_fabsf(a))
                lfo_touch = fm_fabsf(a);
        }
    }

    /* ---- the gestures: the black keys held or latched, and the SEQ step's */
    {
        uint32_t on = latched;
        for (i = 0; i < NBLACK; i++)
            if (keys >> BKEY[i] & 1u)
                on |= 1u << i;
        eng.gest_on = on;
        eng.latched = latched;
        for (i = 0; i < NBLACK + 1u; i++) {
            gest_t *g = &gs[i];
            keyp_t *kp = i < NBLACK ? &patch.key[BKEY[i]] : 0;
            int type = kp ? kp->gest : seq_g - 1, abs_;
            float r, ghz, ox, oy, sp;
            int active = i < NBLACK ? (int)(on >> i & 1u) : seq_g > 0;
            if (!active) {
                g->on = 0;
                continue;
            }
            if (!g->on) {                        /* starts: its anchor */
                g->on = 1;
                g->ph = 0.0f;
                g->vx = g->vy = g->wx = g->wy = 0.0f;
                g->jx = 0.0f;
                g->jy = 0.0f;
                g->ax = has_base ? tx : cx;
                g->ay = has_base ? ty : cy;
            }
            if (kp && kp->x != 255) {
                g->ax = (float)kp->x * 0.01f;
                g->ay = (float)kp->y * 0.01f;
            } else if (has_base) {
                g->ax = tx;
                g->ay = ty;
            }
            sp = (float)(kp && kp->speed ? kp->speed - 1 : patch.gspeed);
            ghz = sp < 5.0f ? hz / (4.0f * GSPEED_BARS[(int)sp]) : GSPEED_HZ[(int)sp - 5 < 6 ? (int)sp - 5 : 5];
            r = 0.5f * (float)(kp && kp->size ? kp->size : patch.gsize) * 0.01f;
            g->ph += ghz * dt;
            if (type == G_JUMP && g->ph >= 1.0f) {
                g->wx = (frnd() * 2.0f - 1.0f) * r;
                g->wy = (frnd() * 2.0f - 1.0f) * r;
            }
            if (g->ph >= 1.0f)
                g->ph -= fm_floorf(g->ph);
            abs_ = gest_shape(g, type, r, dt, ghz, &ox, &oy);
            if (abs_) {
                gx += ox - g->ax;
                gy += oy - g->ay;
            } else {
                gx += ox;
                gy += oy;
            }
            if (!has_base) {
                tx = g->ax;
                ty = g->ay;
                has_base = 1;
            }
            {
                float gt = 0.8f * (kp ? (float)kp->intens * 0.01f : 1.0f);
                if (touch < gt)
                    touch = gt;
            }
            if (i < NBLACK)
                live = 1;
        }
    }
    if (touch < lfo_touch)
        touch = lfo_touch;
    eng.hold = hold_key;

    /* ---- the cursor: the base glides; gestures and LFOs ride on it; the effect fades in and out */
    if (touch > 0.0f || has_base) {
        float c = slew_c(dt, slew_ms);
        bx += (fclamp01(tx) - bx) * c;
        by += (fclamp01(ty) - by) * c;
        amt += (touch - amt) * slew_c(dt, (float)patch.attack_ms);
    } else {
        float rel = (float)patch.release_ms;
        if (patch.rel_mode == 2) {               /* RETURN: back to the centre, slowly */
            float c = slew_c(dt, rel);
            bx += (0.5f - bx) * c;
            by += (0.5f - by) * c;
            amt += (0.0f - amt) * slew_c(dt, rel * 3.0f);
        } else if (patch.rel_mode == 0) {        /* FADE */
            amt += (0.0f - amt) * slew_c(dt, rel);
        }                                        /* STAY: all as it was */
    }
    cx = fclamp01(bx + gx + lfo_x);
    cy = fclamp01(by + gy + lfo_y);
    eng.cx = (int32_t)(cx * 1000.0f);
    eng.cy = (int32_t)(cy * 1000.0f);
    eng.amt = (int32_t)(amt * 1000.0f);

    /* ---- the destinations: bases, then the LFOs and the drift, then the pad */
    for (i = 0; i < ND; i++)
        p[i] = 0.0f;
    p[D_CUTOFF] = (float)patch.fx[FX_VCF].amt * 0.01f;
    p[D_RESO] = (float)patch.fx[FX_VCF].a * 0.01f;
    p[D_MORPH] = 0.35f;
    p[D_PITCH] = 0.5f;
    p[D_FX] = (float)patch.fx_level * 0.01f;
    p[D_DRIVE] = patch.fx[FX_DRIVE].on ? (float)patch.fx[FX_DRIVE].amt * 0.01f : 0.0f;
    p[D_FOLD] = patch.fx[FX_FOLD].on ? (float)patch.fx[FX_FOLD].amt * 0.01f : 0.0f;
    p[D_RING] = patch.fx[FX_RING].on ? (float)patch.fx[FX_RING].amt * 0.01f : 0.0f;
    p[D_CRUSH] = patch.fx[FX_CRUSH].on ? (float)patch.fx[FX_CRUSH].amt * 0.01f : 0.0f;
    p[D_CHORUS] = patch.fx[FX_CHORUS].on ? (float)patch.fx[FX_CHORUS].amt * 0.01f : 0.0f;
    p[D_DELAY] = patch.fx[FX_DELAY].on ? (float)patch.fx[FX_DELAY].amt * 0.01f : 0.0f;
    p[D_DFDBK] = (float)patch.fx[FX_DELAY].b * 0.01f;
    p[D_REVERB] = patch.fx[FX_REVERB].on ? (float)patch.fx[FX_REVERB].amt * 0.01f : 0.0f;
    p[D_VCA] = patch.fx[FX_VCA].on ? (float)patch.fx[FX_VCA].amt * 0.01f : 0.0f;
    for (i = 0; i < SYN_NOSC; i++) {
        p[D_VOL1 + i] = (float)clamp(vol[i], 0, 100) * 0.01f;
        p[D_PIT1 + i] = 0.5f;
    }
    p[D_LFO1] = p[D_LFO2] = p[D_CLOCK] = 0.5f;
    for (i = 0; i < 2; i++) {
        uint8_t d = patch.lfo[i].dest;
        if (d > D_NONE && d < D_CURX)
            p[d] += eng.lfo[i] * (float)patch.lfo[i].amt * 0.005f;
    }
    {
        float d = (float)patch.drift * 0.01f;
        p[D_CUTOFF] += drift_v[2] * d * 0.2f;
        p[D_XMOD] += fm_fabsf(drift_v[3]) * d * 0.35f;
        p[D_MORPH] += drift_v[4] * d * 0.3f;
    }
    pre_xmod = fclamp01(p[D_XMOD]);
    pre_pitch = p[D_PITCH];
    /* the pad's destinations: EDIT's, or the touching key's own */
    xd[0] = patch.xdest[0];
    xd[1] = patch.xdest[1];
    yd[0] = patch.ydest[0];
    yd[1] = patch.ydest[1];
    xa[0] = patch.xamt[0];
    xa[1] = patch.xamt[1];
    ya[0] = patch.yamt[0];
    ya[1] = patch.yamt[1];
    for (i = 0; i < (uint32_t)norder; i++) {
        keyp_t *kp = &patch.key[WKEY[order[i]]];
        if (kp->xdest) {
            xd[0] = kp->xdest;
            xa[0] = kp->xamt;
            xd[1] = D_NONE;
        }
        if (kp->ydest) {
            yd[0] = kp->ydest;
            ya[0] = kp->yamt;
            yd[1] = D_NONE;
        }
    }
    for (i = 0; i < NBLACK; i++)
        if (eng.gest_on >> i & 1u) {
            keyp_t *kp = &patch.key[BKEY[i]];
            mask |= kp->mask;
            if (kp->xdest) {
                xd[0] = kp->xdest;
                xa[0] = kp->xamt;
                xd[1] = D_NONE;
            }
            if (kp->ydest) {
                yd[0] = kp->ydest;
                ya[0] = kp->yamt;
                yd[1] = D_NONE;
            }
        }
    if (!mask)
        mask = 0xF;
    switch (patch.curve) {                       /* LINEAR, SOFT, EXP */
    default: curve_x = cx; curve_y = cy; break;
    case 1:
        curve_x = cx * cx * (3.0f - 2.0f * cx);
        curve_y = cy * cy * (3.0f - 2.0f * cy);
        break;
    case 2: curve_x = cx * cx; curve_y = cy * cy; break;
    }
    {
        float w0 = amt * (float)patch.range * 0.01f;
        int j;
        for (j = 0; j < 2; j++) {
            if (xd[j] > D_NONE && xd[j] < ND_PAD && xa[j]) {
                float w = w0 * fm_fabsf((float)xa[j]) * 0.01f, pos = xa[j] < 0 ? 1.0f - curve_x : curve_x;
                p[xd[j]] += (pos - p[xd[j]]) * w;
            }
            if (yd[j] > D_NONE && yd[j] < ND_PAD && ya[j]) {
                float w = w0 * fm_fabsf((float)ya[j]) * 0.01f, pos = ya[j] < 0 ? 1.0f - curve_y : curve_y;
                p[yd[j]] += (pos - p[yd[j]]) * w;
            }
        }
    }
    for (i = 0; i < ND; i++) {
        p[i] = fclamp01(p[i]);
        eng.p[i] = pv[i] = p[i];
    }

    /* ---- into the sound */
    sc = fm_minf(2.0f * p[D_FX], 2.0f);
    for (i = 0; i < SYN_NOSC; i++) {
        float v = p[D_VOL1 + i], pit = (mask >> i & 1u) ? p[D_PITCH] : pre_pitch;
        syn_in.wave[i] = patch.wave[i];
        syn_in.note[i] = (float)(SYN_BASE_NOTE + patch.semi[i] + semi_seq[i]) + (float)patch.fine[i] * 0.01f +
                         (pit - 0.5f) * 24.0f + (p[D_PIT1 + i] - 0.5f) * 24.0f;
        syn_in.gain[i] = mute_seq[i] ? 0.0f : v * fm_sqrtf(v);
        syn_in.xm[i] = (mask >> i & 1u) ? p[D_XMOD] : pre_xmod;
    }
    syn_in.algo = patch.algo;
    syn_in.morph = p[D_MORPH];
    syn_in.vcf_on = patch.fx[FX_VCF].on;
    syn_in.vcf_mode = patch.fx[FX_VCF].b % 3u;
    syn_in.cutoff = 30.0f * fm_exp2f(p[D_CUTOFF] * 9.05f);
    syn_in.reso = p[D_RESO];
    eng.cutoff_hz = (int32_t)(patch.fx[FX_VCF].on ? syn_in.cutoff : 16000.0f);
    syn_in.drive = fm_minf(p[D_DRIVE] * sc, 1.0f);
    syn_in.drive_tone = (float)patch.fx[FX_DRIVE].a * 0.01f;
    syn_in.fold = fm_minf(p[D_FOLD] * sc, 1.0f);
    syn_in.fold_sym = (float)patch.fx[FX_FOLD].a * 0.02f - 1.0f;
    syn_in.ring = fm_minf(p[D_RING] * sc, 1.0f);
    syn_in.ring_hz = 20.0f * fm_exp2f((float)patch.fx[FX_RING].a * 0.0664f);
    syn_in.crush_bits = fm_minf(p[D_CRUSH] * sc, 1.0f);
    syn_in.crush_rate = patch.fx[FX_CRUSH].on ? (float)patch.fx[FX_CRUSH].a * 0.01f : 0.0f;
    syn_in.chorus = fm_minf(p[D_CHORUS] * sc, 1.0f);
    syn_in.chorus_rate = 0.05f * fm_exp2f((float)patch.fx[FX_CHORUS].a * 0.0664f);
    syn_in.chorus_depth = (float)patch.fx[FX_CHORUS].b * 0.01f;
    syn_in.delay = fm_minf(p[D_DELAY] * sc, 1.0f);
    syn_in.delay_s = 1.0f / (hz * RATE_SPQ[patch.fx[FX_DELAY].a % NRATE]);
    syn_in.delay_fb = p[D_DFDBK] * 0.95f;
    syn_in.reverb = fm_minf(p[D_REVERB] * sc, 1.0f);
    syn_in.rev_size = (float)patch.fx[FX_REVERB].a * 0.01f;
    syn_in.rev_damp = (float)patch.fx[FX_REVERB].b * 0.01f;
    if (fx_bypass) {
        syn_in.drive = syn_in.fold = syn_in.ring = syn_in.crush_bits = syn_in.crush_rate = 0.0f;
        syn_in.chorus = syn_in.delay = syn_in.reverb = 0.0f;
        p[D_VCA] = 0.0f;
    }
    {   /* VCA: a tremolo in time with the clock */
        float u;
        vca_ph += hz * RATE_SPQ[patch.fx[FX_VCA].a % NRATE] * dt;
        vca_ph -= fm_floorf(vca_ph);
        u = patch.fx[FX_VCA].b == 1 ? (vca_ph < 0.5f ? 1.0f : 0.0f)
            : patch.fx[FX_VCA].b == 2 ? 1.0f - vca_ph
                                      : 0.5f + 0.5f * fm_sin_turns(vca_ph);
        syn_in.vca = 1.0f - p[D_VCA] * (1.0f - u);
    }

    /* ---- MIDI out: the cursor and the knobs as CCs (SYSTEM > CC OUT) */
    cc_t += dt;
    if (sys.cc_out && cc_t >= 0.02f) {
        int32_t v[6];
        cc_t = 0.0f;
        v[0] = eng.cx * 127 / 1000;
        v[1] = eng.cy * 127 / 1000;
        for (i = 0; i < SYN_NOSC; i++)
            v[2 + i] = vol[i] * 127 / 100;
        for (i = 0; i < 6; i++)
            if (v[i] != midi_cc_last[i]) {
                uint32_t ch = sys.midi_ch ? sys.midi_ch - 1u : 0u;
                midi_cc_last[i] = v[i];
                plat_midi_send(0x0Bu | (0xB0u | ch) << 8 | (12u + i) << 16 | (uint32_t)v[i] << 24, TO_USB);
            }
    }
}
