/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Hirakos */
/* MODELAR: the sound (synth.h). Its own unit, compiled at -O2. */
#include "synth.h"
#include "fastmath.h"

#if defined PLAT_HOST
#define SYN_POOL
#else
#define SYN_POOL __attribute__((section(".pool")))
#endif

const char *const SYN_WAVE_NAME[SYN_NWAVES] = {"SINE", "TRIANGLE", "SAW", "SQUARE", "PULSE", "SUPERSAW",
                                               "NOISE", "S&H", "MORPH", "FM 2-OP", "FORMANT", "CHAOS"};
const char *const SYN_WAVE_SHORT[SYN_NWAVES] = {"SIN", "TRI", "SAW", "SQR", "PLS", "SSW",
                                                "NOI", "S&H", "MRF", "FM2", "FRM", "CHS"};
const char *const SYN_ALGO_NAME[SYN_NALGOS] = {"MIX", "PAIRS FM", "CHAIN FM", "RING", "SYNC", "CROSS", "AM",
                                               "FEEDBACK"};

syn_in_t syn_in;
volatile syn_out_t syn_out;

typedef struct {
    float ph;                            /* 0..1 */
    float inc, fmul;                     /* phase step a sample; SYNC's ratio for the slaves */
    float ss[2];                         /* SUPERSAW's two detuned partners */
    float held;                          /* S&H's and CHAOS's value, NOISE's low-pass */
    float gain, peak;                    /* the volume, glided; the output level for the screen */
    float xm;                            /* the routing's depth, glided */
    float prev;                          /* the last sample (CROSS, FEEDBACK) */
    uint32_t rng;
    int wrapped;                         /* the phase went past 1 this sample (SYNC) */
} osc_t;

static osc_t osc[SYN_NOSC];
static float morph_now;
static float f_a1, f_a2, f_a3, f_k, f_ic1, f_ic2;   /* the filter */
static float gain_now, out_peak;
static int muted;

/* the FX' state */
static float drv_lp, ring_ph, crush_hold, vca_now;
static uint32_t crush_n;
static float mix_s[8];                   /* glided: drive fold ring chorus delay reverb vca (spare) */
#define CH_N 2048u                       /* the chorus' line: 46 ms */
static float ch_buf[CH_N] SYN_POOL;
static uint32_t ch_w;
static float ch_ph;
#define DL_N 65536u                      /* the delay's line: 1.48 s, 16-bit */
static int16_t dl_buf[DL_N] SYN_POOL;
static uint32_t dl_w;
static float dl_time;                    /* glided, in samples: a change of time bends like tape */
static float dl_lp;
#define RV_NC 4
#define RV_NA 2
static const uint16_t RV_COMB[RV_NC] = {1116, 1188, 1277, 1356}, RV_AP[RV_NA] = {556, 441};
#define RV_SPREAD 23
static float rv_c[2][RV_NC][1356 + RV_SPREAD] SYN_POOL;
static float rv_a[2][RV_NA][556 + RV_SPREAD] SYN_POOL;
static uint16_t rv_ci[2][RV_NC], rv_ai[2][RV_NA];
static float rv_lp[2][RV_NC];

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}
static float frand(uint32_t *s) { return (float)(xorshift(s) >> 8) * (2.0f / 16777216.0f) - 1.0f; }

static float blep(float t, float dt)     /* PolyBLEP: the step's correction near a discontinuity */
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}
static float wrap1(float p) { return p - fm_floorf(p); }

static float shape(int32_t w, float t, float dt, float morph, osc_t *o)
{
    float v, a, b;
    switch (w) {
    default:
    case W_SINE:
        return fm_sin_turns(t);
    case W_TRI:
        return 4.0f * fm_fabsf(t - 0.5f) - 1.0f;
    case W_SAW:
        return 2.0f * t - 1.0f - blep(t, dt);
    case W_SQUARE:
        return (t < 0.5f ? 1.0f : -1.0f) + blep(t, dt) - blep(wrap1(t + 0.5f), dt);
    case W_PULSE: {
        float pw = 0.5f - 0.42f * morph;
        return (t < pw ? 1.0f : -1.0f) + blep(t, dt) - blep(wrap1(t + 1.0f - pw), dt);
    }
    case W_SSAW:
        v = 2.0f * t - 1.0f - blep(t, dt);
        if (o) {
            v += 2.0f * o->ss[0] - 1.0f - blep(o->ss[0], dt);
            v += 2.0f * o->ss[1] - 1.0f - blep(o->ss[1], dt);
        } else {
            v += 2.0f * wrap1(t * 1.007f + 0.3f) - 1.0f + 2.0f * wrap1(t * 0.993f + 0.6f) - 1.0f;
        }
        return v * 0.55f;
    case W_NOISE:
        if (o)
            return o->held;
        {
            uint32_t seed = (uint32_t)(t * 977.0f) * 2654435761u | 1u;   /* the same picture every time */
            return frand(&seed) * 0.8f;
        }
    case W_SH:
    case W_CHAOS:
        return o ? o->held : (t < 0.25f ? 0.6f : t < 0.5f ? -0.2f : t < 0.75f ? 0.9f : -0.7f);
    case W_MORPH:                        /* sine -> triangle -> saw -> square, by morph */
        a = morph * 3.0f;
        if (a < 1.0f) {
            v = fm_sin_turns(t);
            b = 4.0f * fm_fabsf(t - 0.5f) - 1.0f;
        } else if (a < 2.0f) {
            a -= 1.0f;
            v = 4.0f * fm_fabsf(t - 0.5f) - 1.0f;
            b = 2.0f * t - 1.0f - blep(t, dt);
        } else {
            a -= 2.0f;
            v = 2.0f * t - 1.0f - blep(t, dt);
            b = (t < 0.5f ? 1.0f : -1.0f) + blep(t, dt) - blep(wrap1(t + 0.5f), dt);
        }
        return v + (b - v) * a;
    case W_FM2:                          /* a sine, modulated by its own octave */
        return fm_sin_turns(t + (0.08f + 0.3f * morph) * fm_sin_turns(2.0f * t));
    case W_FORMANT:                      /* a sine burst at 2..8 times the pitch, fading over each cycle */
        return fm_sin_turns(t * (2.0f + 6.0f * morph)) * (1.0f - t);
    }
}

float syn_wave_preview(int32_t wave, float ph, float morph) { return shape(wave, wrap1(ph), 0.0f, morph, 0); }
float syn_osc_hz(int i) { return osc[i & 3].inc * SYN_SR; }

/* one oscillator, one sample: pm (turns) bends its phase, without changing where it is */
static float tick(int i, int32_t w, float pm, float morph)
{
    osc_t *o = &osc[i];
    float inc = o->inc * o->fmul, t;
    o->ph += inc;
    o->wrapped = o->ph >= 1.0f;
    if (o->wrapped) {
        o->ph -= 1.0f;
        if (w == W_SH)
            o->held = frand(&o->rng);
        else if (w == W_CHAOS) {         /* the logistic map, one step a cycle, at the edge of chaos */
            float x = 0.5f + 0.5f * o->held, r = 3.57f + 0.42f * morph;
            x = r * x * (1.0f - x);
            o->held = fm_clampf(2.0f * x - 1.0f, -1.0f, 1.0f);
        }
    }
    if (w == W_SSAW) {
        o->ss[0] = wrap1(o->ss[0] + inc * 1.0071f);
        o->ss[1] = wrap1(o->ss[1] + inc * 0.9931f);
    } else if (w == W_NOISE) {           /* white noise through a low-pass that follows the pitch */
        float c = fm_minf(inc * 6.0f, 1.0f);
        o->held += (frand(&o->rng) - o->held) * c;
    }
    t = pm != 0.0f ? wrap1(o->ph + pm) : o->ph;
    return shape(w, t, inc, morph, o);
}

void syn_init(void)
{
    int i;
    for (i = 0; i < SYN_NOSC; i++) {
        osc[i].rng = 0x9E3779B9u * (uint32_t)(i + 1);
        osc[i].ph = 0.25f * (float)i;
        osc[i].ss[0] = 0.33f;
        osc[i].ss[1] = 0.66f;
        osc[i].fmul = 1.0f;
        osc[i].held = 0.3f;
        osc[i].inc = 0.001f;
    }
    f_a1 = 1.0f;
    f_k = 1.8f;
    vca_now = 1.0f;
    mix_s[6] = 1.0f;
    dl_time = 11025.0f;
    muted = 0;
}

void syn_panic(void)
{
    int i;
    muted = 1;
    gain_now = 0.0f;
    for (i = 0; i < SYN_NOSC; i++)
        osc[i].gain = 0.0f;
}

static float rv_tick(int c, float in, float fb, float damp)   /* Freeverb: four combs, two all-passes */
{
    float out = 0.0f;
    int j;
    for (j = 0; j < RV_NC; j++) {
        uint32_t n = RV_COMB[j] + (c ? RV_SPREAD : 0), k = rv_ci[c][j];
        float y = rv_c[c][j][k];
        rv_lp[c][j] = fm_flush(y + (rv_lp[c][j] - y) * damp);
        rv_c[c][j][k] = in + rv_lp[c][j] * fb;
        rv_ci[c][j] = (uint16_t)(k + 1u >= n ? 0u : k + 1u);
        out += y;
    }
    for (j = 0; j < RV_NA; j++) {
        uint32_t n = RV_AP[j] + (c ? RV_SPREAD : 0), k = rv_ai[c][j];
        float b = rv_a[c][j][k];
        rv_a[c][j][k] = fm_flush(out + b * 0.5f);
        rv_ai[c][j] = (uint16_t)(k + 1u >= n ? 0u : k + 1u);
        out = b - out;
    }
    return out;
}

static float dl_read(float back)         /* the delay line, back samples ago, interpolated */
{
    float r = (float)dl_w - back + (float)DL_N, f;
    uint32_t i = (uint32_t)r;
    f = r - (float)i;
    return ((float)dl_buf[i & (DL_N - 1u)] * (1.0f - f) + (float)dl_buf[(i + 1u) & (DL_N - 1u)] * f) *
           (1.0f / 32768.0f);
}

static float ch_read(float back)
{
    float r = (float)ch_w - back + (float)CH_N, f;
    uint32_t i = (uint32_t)r;
    f = r - (float)i;
    return ch_buf[i & (CH_N - 1u)] * (1.0f - f) + ch_buf[(i + 1u) & (CH_N - 1u)] * f;
}

void syn_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    const syn_in_t *p = &syn_in;
    uint32_t s;
    int i;
    float gtarget[SYN_NOSC], g, k, a1, a2, a3, da1, da2, da3, dk, inv = 1.0f / (float)n, fc;
    float gain = muted ? 0.0f : (float)master_q12 / 4096.0f * 8300000.0f;
    float tgt[8], drv_g, drv_norm, drv_c, fold_g, crush_q, ring_inc, ch_inc, ch_dep, dl_target, dl_fb, rv_fb, rv_damp;
    int32_t algo = clampi(p->algo, 0, SYN_NALGOS - 1), wv[SYN_NOSC], crush_every, vmode = p->vcf_mode;

    /* the filter's coefficients for the block's end, glided from where the last block left them */
    fc = p->vcf_on ? fm_clampf(p->cutoff, 30.0f, 16000.0f) : 16000.0f;
    g = fm_tanf(FM_PI * fc / SYN_SR);
    k = 2.0f - 1.9f * fm_clampf(p->vcf_on ? p->reso : 0.1f, 0.0f, 1.0f);
    if (!p->vcf_on)
        vmode = VCF_LP;
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
    da1 = (a1 - f_a1) * inv;
    da2 = (a2 - f_a2) * inv;
    da3 = (a3 - f_a3) * inv;
    dk = (k - f_k) * inv;

    for (i = 0; i < SYN_NOSC; i++) {
        float hz = 440.0f * fm_exp2f((fm_clampf(p->note[i], 0.0f, 120.0f) - 69.0f) * (1.0f / 12.0f));
        wv[i] = clampi(p->wave[i], 0, SYN_NWAVES - 1);
        osc[i].inc = fm_minf(hz, 18000.0f) / SYN_SR;
        osc[i].fmul = algo == A_SYNC && i > 0 ? 1.0f + 3.0f * fm_clampf(p->xm[i], 0.0f, 1.0f) : 1.0f;
        gtarget[i] = muted ? 0.0f : fm_clampf(p->gain[i], 0.0f, 1.0f);
        osc[i].peak *= 0.8f;
    }
    out_peak *= 0.8f;

    tgt[0] = fm_clampf(p->drive, 0.0f, 1.0f);
    tgt[1] = fm_clampf(p->fold, 0.0f, 1.0f);
    tgt[2] = fm_clampf(p->ring, 0.0f, 1.0f);
    tgt[3] = fm_clampf(p->chorus, 0.0f, 1.0f);
    tgt[4] = fm_clampf(p->delay, 0.0f, 1.0f);
    tgt[5] = fm_clampf(p->reverb, 0.0f, 1.0f);
    tgt[6] = fm_clampf(p->vca, 0.0f, 1.0f);
    tgt[7] = 0.0f;
    drv_g = 1.0f + 12.0f * mix_s[0];
    drv_norm = 1.0f / fm_tanhf(drv_g);
    drv_c = 1.0f - fm_expf(-FM_TWO_PI * (800.0f * fm_exp2f(4.3f * fm_clampf(p->drive_tone, 0.0f, 1.0f))) / SYN_SR);
    fold_g = 1.0f + 6.0f * mix_s[1];
    crush_q = p->crush_bits > 0.0f ? fm_exp2f(-(15.0f - 13.0f * fm_clampf(p->crush_bits, 0.0f, 1.0f))) : 0.0f;
    crush_every = p->crush_rate > 0.0f ? 1 + (int32_t)(fm_clampf(p->crush_rate, 0.0f, 1.0f) * 31.0f) : 1;
    ring_inc = fm_clampf(p->ring_hz, 1.0f, 5000.0f) / SYN_SR;
    ch_inc = fm_clampf(p->chorus_rate, 0.02f, 8.0f) / SYN_SR;
    ch_dep = 1.0f + 350.0f * fm_clampf(p->chorus_depth, 0.0f, 1.0f);
    dl_target = fm_clampf(p->delay_s, 0.01f, 1.45f) * SYN_SR;
    dl_fb = fm_clampf(p->delay_fb, 0.0f, 0.95f);
    rv_fb = 0.7f + 0.28f * fm_clampf(p->rev_size, 0.0f, 1.0f);
    rv_damp = 0.05f + 0.6f * fm_clampf(p->rev_damp, 0.0f, 1.0f);

    for (s = 0; s < n; s++) {
        float o[SYN_NOSC], d[SYN_NOSC], mix, x, l, r, v1, v2, v3;
        morph_now += (p->morph - morph_now) * 0.003f;
        for (i = 0; i < SYN_NOSC; i++) {
            osc[i].xm += (fm_clampf(p->xm[i], 0.0f, 1.0f) - osc[i].xm) * 0.003f;
            d[i] = 0.5f * osc[i].xm;     /* modulation depth, in turns */
        }
        for (i = 0; i < 7; i++)
            mix_s[i] += (tgt[i] - mix_s[i]) * 0.002f;
        switch (algo) {
        default:
        case A_MIX:
        case A_RING:
        case A_AM:
            for (i = 0; i < SYN_NOSC; i++)
                o[i] = tick(i, wv[i], 0.0f, morph_now);
            break;
        case A_PAIRS:
            o[1] = tick(1, wv[1], 0.0f, morph_now);
            o[0] = tick(0, wv[0], d[0] * o[1], morph_now);
            o[3] = tick(3, wv[3], 0.0f, morph_now);
            o[2] = tick(2, wv[2], d[2] * o[3], morph_now);
            break;
        case A_CHAIN:
            o[3] = tick(3, wv[3], 0.0f, morph_now);
            o[2] = tick(2, wv[2], d[2] * o[3], morph_now);
            o[1] = tick(1, wv[1], d[1] * o[2], morph_now);
            o[0] = tick(0, wv[0], d[0] * o[1], morph_now);
            break;
        case A_SYNC:                                     /* 2, 3, 4 restart with 1, faster as their depth rises */
            o[0] = tick(0, wv[0], 0.0f, morph_now);
            if (osc[0].wrapped)
                for (i = 1; i < SYN_NOSC; i++)
                    osc[i].ph = wrap1(osc[0].ph * osc[i].inc * osc[i].fmul / osc[0].inc);
            for (i = 1; i < SYN_NOSC; i++)
                o[i] = tick(i, wv[i], 0.0f, morph_now);
            break;
        case A_CROSS:                                    /* 1 <-> 2 and 3 <-> 4, each by the other's last sample */
            o[0] = tick(0, wv[0], d[0] * osc[1].prev, morph_now);
            o[1] = tick(1, wv[1], d[1] * osc[0].prev, morph_now);
            o[2] = tick(2, wv[2], d[2] * osc[3].prev, morph_now);
            o[3] = tick(3, wv[3], d[3] * osc[2].prev, morph_now);
            break;
        case A_FEEDBACK:                                 /* 1 -> 2 -> 3 -> 4 -> 1, a ring */
            o[0] = tick(0, wv[0], 1.3f * d[0] * osc[3].prev, morph_now);
            o[1] = tick(1, wv[1], 1.3f * d[1] * o[0], morph_now);
            o[2] = tick(2, wv[2], 1.3f * d[2] * o[1], morph_now);
            o[3] = tick(3, wv[3], 1.3f * d[3] * o[2], morph_now);
            break;
        }
        if (algo == A_RING) {
            float r12 = o[0] * o[1], r34 = o[2] * o[3];
            o[0] += (r12 - o[0]) * osc[0].xm;
            o[1] += (r12 - o[1]) * osc[1].xm;
            o[2] += (r34 - o[2]) * osc[2].xm;
            o[3] += (r34 - o[3]) * osc[3].xm;
        } else if (algo == A_AM) {
            for (i = 1; i < SYN_NOSC; i++)
                o[i] *= 1.0f - osc[i].xm + osc[i].xm * (0.5f + 0.5f * o[0]);
        }
        mix = 0.0f;
        for (i = 0; i < SYN_NOSC; i++) {
            osc_t *q = &osc[i];
            float out;
            q->prev = o[i];
            q->gain += (gtarget[i] - q->gain) * 0.002f;  /* ~10 ms: turning a knob does not click */
            out = o[i] * q->gain;
            mix += out;
            if (fm_fabsf(out) > q->peak)
                q->peak = fm_fabsf(out);
        }
        /* VCF: a state-variable filter (TPT), its coefficients glided across the block */
        f_a1 += da1;
        f_a2 += da2;
        f_a3 += da3;
        f_k += dk;
        x = mix * 0.6f;
        v3 = x - f_ic2;
        v1 = f_a1 * f_ic1 + f_a2 * v3;
        v2 = f_ic2 + f_a2 * f_ic1 + f_a3 * v3;
        f_ic1 = fm_flush(2.0f * v1 - f_ic1);
        f_ic2 = fm_flush(2.0f * v2 - f_ic2);
        x = vmode == VCF_BP ? v1 : vmode == VCF_HP ? x - f_k * v1 - v2 : v2;
        /* DRIVE, FOLD, RING, CRUSH, VCA: mono */
        if (mix_s[0] > 0.001f) {
            float y = fm_tanhf(x * drv_g) * drv_norm;
            drv_lp += (y - drv_lp) * drv_c;
            x += (drv_lp - x) * fm_minf(1.0f, mix_s[0] * 4.0f);
        }
        if (mix_s[1] > 0.001f) {
            float y = fm_sin_turns((fold_g * x + 0.5f * p->fold_sym) * 0.25f);
            x += (y - x) * fm_minf(1.0f, mix_s[1] * 4.0f);
        }
        ring_ph = wrap1(ring_ph + ring_inc);
        if (mix_s[2] > 0.001f)
            x += (x * fm_sin_turns(ring_ph) - x) * mix_s[2];
        if (crush_q > 0.0f || crush_every > 1) {
            if (++crush_n >= (uint32_t)crush_every) {
                crush_n = 0;
                crush_hold = crush_q > 0.0f ? fm_floorf(x / crush_q + 0.5f) * crush_q : x;
            }
            x = crush_hold;
        }
        vca_now += (mix_s[6] - vca_now) * 0.01f;
        x *= vca_now;
        /* CHORUS: two taps swaying a quarter apart; from here on, stereo */
        ch_buf[ch_w] = x;
        ch_ph = wrap1(ch_ph + ch_inc);
        l = r = x;
        if (mix_s[3] > 0.001f) {
            float tl = ch_read(530.0f + ch_dep * fm_sin_turns(ch_ph)), tr = ch_read(530.0f + ch_dep * fm_sin_turns(ch_ph + 0.25f));
            l = x * (1.0f - 0.5f * mix_s[3]) + tl * mix_s[3] * 0.8f;
            r = x * (1.0f - 0.5f * mix_s[3]) + tr * mix_s[3] * 0.8f;
        }
        ch_w = (ch_w + 1u) & (CH_N - 1u);
        /* DELAY: a 16-bit line, its repeats darker; the right tap at three quarters of the time */
        dl_time += (dl_target - dl_time) * 0.0004f;
        {
            float tl = dl_read(dl_time), tr = dl_read(dl_time * 0.75f), w;
            dl_lp += (tl - dl_lp) * 0.35f;
            w = fm_clampf(0.5f * (l + r) * mix_s[4] + dl_lp * dl_fb, -1.0f, 1.0f);
            dl_buf[dl_w] = (int16_t)(w * 32000.0f);
            dl_w = (dl_w + 1u) & (DL_N - 1u);
            l += tl * mix_s[4];
            r += tr * mix_s[4];
        }
        /* REVERB */
        if (mix_s[5] > 0.001f) {
            float in = (l + r) * 0.08f;
            float rl = rv_tick(0, in, rv_fb, rv_damp), rr = rv_tick(1, in, rv_fb, rv_damp);
            l = l * (1.0f - 0.4f * mix_s[5]) + rl * mix_s[5];
            r = r * (1.0f - 0.4f * mix_s[5]) + rr * mix_s[5];
        }
        l = fm_tanhf(l) * 0.92f;
        r = fm_tanhf(r) * 0.92f;
        if (fm_fabsf(l) > out_peak)
            out_peak = fm_fabsf(l);
        gain_now += (gain - gain_now) * 0.004f;
        lr[2 * s] = (int32_t)(l * gain_now);
        lr[2 * s + 1] = (int32_t)(r * gain_now);
    }
    f_a1 = a1;                                           /* (no drift from the steps) */
    f_a2 = a2;
    f_a3 = a3;
    f_k = k;
    for (i = 0; i < SYN_NOSC; i++)
        syn_out.level[i] = (int32_t)fm_minf(osc[i].peak * 1000.0f, 1000.0f);
    syn_out.out_level = (int32_t)fm_minf(out_peak * 1000.0f, 1000.0f);
}
