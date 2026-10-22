/* MODELAR: the instrument (synth.h). Its own unit, compiled at -O2. */
#include "synth.h"
#include "fastmath.h"

const char *const SYN_WAVE_NAME[SYN_NWAVES] = {"SINE", "TRIANGLE", "SAW", "SQUARE", "PULSE", "SUPERSAW",
                                               "NOISE", "S&H", "MORPH", "FM 2-OP", "FORMANT", "CHAOS"};
const char *const SYN_WAVE_SHORT[SYN_NWAVES] = {"SIN", "TRI", "SAW", "SQR", "PLS", "SSW",
                                                "NOI", "S&H", "MRF", "FM2", "FRM", "CHS"};
const char *const SYN_ALGO_NAME[SYN_NALGOS] = {"MIX", "PAIRS FM", "CHAIN FM", "RING", "SYNC", "CROSS", "AM",
                                               "FEEDBACK"};

volatile syn_in_t syn_in;
volatile syn_out_t syn_out;

typedef struct {
    float ph;                            /* 0..1 */
    float inc, fmul;                     /* phase step a sample; SYNC's ratio for the slaves */
    float ss[2];                         /* SUPERSAW's two detuned partners */
    float held;                          /* S&H's and CHAOS's value, NOISE's low-pass */
    float gain, peak;                    /* the volume, glided; the output level for the screen */
    float prev;                          /* the last sample (CROSS, FEEDBACK) */
    uint32_t rng;
    int wrapped;                         /* the phase went past 1 this sample (SYNC) */
} osc_t;

static osc_t osc[SYN_NOSC];
static float cx = 0.5f, cy = 0.5f, amt;  /* the cursor, the pad's effect */
static float xm_now, morph_now;          /* glided a sample at a time */
static float f_a1, f_a2, f_a3;           /* the filter's coefficients now */
static float f_ic1, f_ic2;
static float gain_now;
static int muted;

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
    }
    f_a1 = 1.0f;
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

static float coef(float dt, float ms) { return ms <= 0.0f ? 1.0f : 1.0f - fm_expf(-dt * 1000.0f / ms); }

void syn_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t s;
    int i;
    float dt = (float)n / SYN_SR, gtarget[SYN_NOSC], range, a, xm, morph, fx, lfc, g, k, a1, a2, a3;
    float da1, da2, da3, inv = 1.0f / (float)n;
    float gain = muted ? 0.0f : (float)master_q12 / 4096.0f * 8300000.0f;
    int32_t algo = syn_in.algo, wv[SYN_NOSC], touch = syn_in.touch;

    /* the pad, once a block: the cursor glides to the keys while they are held and stays where it was let go;
     * the effect fades in on a touch (more keys, more of it) and out on letting go */
    if (touch > 0) {
        float c = coef(dt, (float)syn_in.slew_ms);
        cx += ((float)syn_in.tx * 0.001f - cx) * c;
        cy += ((float)syn_in.ty * 0.001f - cy) * c;
        a = fm_minf(1.0f, 0.7f + 0.15f * (float)(touch - 1));
        amt += (a - amt) * coef(dt, (float)syn_in.attack_ms);
    } else {
        amt += (0.0f - amt) * coef(dt, (float)syn_in.release_ms);
    }
    range = (float)clampi(syn_in.range, 0, 100) * 0.01f;
    a = amt * range;
    xm = a * cy;                                         /* Y: the oscillators into each other */
    morph = 0.35f + (cx - 0.35f) * a;                    /* X: the morphing waves */
    fx = 60.0f * fm_exp2f(cx * 8.0f);                    /* X: the cutoff, 60 Hz .. 15 kHz */
    lfc = 14.0f + (fm_log2f(fx) - 14.0f) * a;            /* untouched: open (16 kHz) */
    g = fm_tanf(FM_PI * fm_minf(fm_exp2f(lfc), 16000.0f) / SYN_SR);
    k = 2.0f - 2.0f * (0.2f + 0.45f * a);                /* more touch, more resonance */
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
    da1 = (a1 - f_a1) * inv;
    da2 = (a2 - f_a2) * inv;
    da3 = (a3 - f_a3) * inv;

    for (i = 0; i < SYN_NOSC; i++) {
        float note = (float)(SYN_BASE_NOTE + clampi(syn_in.semi[i], -48, 48)) + (float)syn_in.fine[i] * 0.01f;
        float v = (float)clampi(syn_in.vol[i], 0, 100) * 0.01f;
        wv[i] = clampi(syn_in.wave[i], 0, SYN_NWAVES - 1);
        osc[i].inc = 440.0f * fm_exp2f((note - 69.0f) * (1.0f / 12.0f)) / SYN_SR;
        osc[i].fmul = algo == A_SYNC && i > 0 ? 1.0f + 3.0f * xm : 1.0f;
        gtarget[i] = muted ? 0.0f : v * fm_sqrtf(v);    /* v^1.5: the knob's middle is clearly heard */
        osc[i].peak *= 0.8f;
    }

    for (s = 0; s < n; s++) {
        float o[SYN_NOSC], d, mix, y, v1, v2, v3;
        xm_now += (xm - xm_now) * 0.003f;
        morph_now += (morph - morph_now) * 0.003f;
        d = 0.5f * xm_now;                               /* modulation depth, in turns */
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
            o[0] = tick(0, wv[0], d * o[1], morph_now);
            o[3] = tick(3, wv[3], 0.0f, morph_now);
            o[2] = tick(2, wv[2], d * o[3], morph_now);
            break;
        case A_CHAIN:
            o[3] = tick(3, wv[3], 0.0f, morph_now);
            o[2] = tick(2, wv[2], d * o[3], morph_now);
            o[1] = tick(1, wv[1], d * o[2], morph_now);
            o[0] = tick(0, wv[0], d * o[1], morph_now);
            break;
        case A_SYNC:                                     /* 2, 3, 4 restart with 1, faster as Y rises */
            o[0] = tick(0, wv[0], 0.0f, morph_now);
            if (osc[0].wrapped)
                for (i = 1; i < SYN_NOSC; i++)
                    osc[i].ph = wrap1(osc[0].ph * osc[i].inc * osc[i].fmul / osc[0].inc);
            for (i = 1; i < SYN_NOSC; i++)
                o[i] = tick(i, wv[i], 0.0f, morph_now);
            break;
        case A_CROSS:                                    /* 1 <-> 2 and 3 <-> 4, each by the other's last sample */
            o[0] = tick(0, wv[0], d * osc[1].prev, morph_now);
            o[1] = tick(1, wv[1], d * osc[0].prev, morph_now);
            o[2] = tick(2, wv[2], d * osc[3].prev, morph_now);
            o[3] = tick(3, wv[3], d * osc[2].prev, morph_now);
            break;
        case A_FEEDBACK:                                 /* 1 -> 2 -> 3 -> 4 -> 1, a ring */
            o[0] = tick(0, wv[0], 1.3f * d * osc[3].prev, morph_now);
            o[1] = tick(1, wv[1], 1.3f * d * o[0], morph_now);
            o[2] = tick(2, wv[2], 1.3f * d * o[1], morph_now);
            o[3] = tick(3, wv[3], 1.3f * d * o[2], morph_now);
            break;
        }
        if (algo == A_RING) {
            float r12 = o[0] * o[1], r34 = o[2] * o[3];
            o[0] += (r12 - o[0]) * xm_now;
            o[1] += (r12 - o[1]) * xm_now;
            o[2] += (r34 - o[2]) * xm_now;
            o[3] += (r34 - o[3]) * xm_now;
        } else if (algo == A_AM) {
            float m = 1.0f - xm_now + xm_now * (0.5f + 0.5f * o[0]);
            o[1] *= m;
            o[2] *= m;
            o[3] *= m;
        }
        mix = 0.0f;
        for (i = 0; i < SYN_NOSC; i++) {
            osc_t *p = &osc[i];
            float out;
            p->prev = o[i];
            p->gain += (gtarget[i] - p->gain) * 0.002f;  /* ~10 ms: turning a knob does not click */
            out = o[i] * p->gain;
            mix += out;
            if (fm_fabsf(out) > p->peak)
                p->peak = fm_fabsf(out);
        }
        /* the filter: a state-variable low-pass (TPT), its coefficients glided across the block */
        f_a1 += da1;
        f_a2 += da2;
        f_a3 += da3;
        v3 = mix - f_ic2;
        v1 = f_a1 * f_ic1 + f_a2 * v3;
        v2 = f_ic2 + f_a2 * f_ic1 + f_a3 * v3;
        f_ic1 = fm_flush(2.0f * v1 - f_ic1);
        f_ic2 = fm_flush(2.0f * v2 - f_ic2);
        y = fm_tanhf(v2 * 0.8f) * 0.9f;                  /* four oscillators at full: a soft ceiling */
        gain_now += (gain - gain_now) * 0.004f;
        lr[2 * s] = lr[2 * s + 1] = (int32_t)(y * gain_now);
    }
    f_a1 = a1;                                           /* (no drift from the steps) */
    f_a2 = a2;
    f_a3 = a3;

    syn_out.cx = (int32_t)(cx * 1000.0f);
    syn_out.cy = (int32_t)(cy * 1000.0f);
    syn_out.amt = (int32_t)(amt * 1000.0f);
    syn_out.xmod = (int32_t)(xm * 1000.0f);
    syn_out.cutoff_hz = (int32_t)fm_exp2f(lfc);
    for (i = 0; i < SYN_NOSC; i++)
        syn_out.level[i] = (int32_t)fm_minf(osc[i].peak * 1000.0f, 1000.0f);
}
