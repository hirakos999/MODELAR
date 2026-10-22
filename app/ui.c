/* MODELAR: the panel and the screen (main loop). Part of the unity build (app.c), after the platform: gfx.c and
 * announce.c are in scope. The map of every control is firmware_hoc/MAPPA_FM1.md; in short:
 *
 *   white keys   the KAOS PAD's 16 points (SEL + key: HOLD there)     black keys  gestures (SEL + key: LATCH)
 *   SELECT       the active oscillator (SEL: the clock's rate)        on a page: its rows
 *   PRESET       its wave (SEL: load a patch)                          on a page: the row's parameters
 *   ALGO         the routing (SEL: the FX' dry/wet)                    on a page: the value
 *   K1..K4       the volumes, always (SEL: fine tune); 0 at power-on and at every patch
 *   OCT- / OCT+  the active oscillator's tone (SEL: an octave); both: back to 0 (SEL: all four)
 *   FX ENV LFO EDIT GLO ARP SEQ: their pages (a tap opens, a tap closes; held: open while held)
 *   HOME KAOS PAD <-> 4 BARS (SEL: SYSTEM)  SAVE the patch (SEL: the slots; held 2 s: quick backup)
 *   PLAY start / stop (SEL: tap the clock)  REC record (SEL: clear the lane; SEL + REC + PLAY: the pattern)
 *   SEL          SHIFT: held, or two clean taps to lock it */
#include "fastmath.h"

int slot_name(int i, char *b);
int slot_save(int i);
int slot_load(int i);
void sys_load(void);
int sys_save(void);

enum { P_KAOS, P_BARS, P_FX, P_ENV, P_LFO, P_EDIT, P_GLO, P_ARP, P_SEQ, P_SAVE, P_SYS, NPAGE };
static const char *const PAGE_NAME[NPAGE] = {"KAOS PAD", "BARS", "FX", "ENV", "LFO", "EDIT", "GLO", "ARP", "SEQ",
                                             "SLOTS", "SYSTEM"};
static uint8_t patch_dirty;

/* ------------------------------------------------------------------- state --- */
static struct {
    int page, main;                      /* the page shown; the main one HOME returns to (P_KAOS or P_BARS) */
    int row[NPAGE], col[NPAGE];          /* where each page was left */
    uint32_t keys, btn;                  /* held at the last input */
    uint8_t shift_lock;                  /* SEL tapped twice: SHIFT stays on (its LED blinks); a tap lets go */
    uint32_t sel_down, sel_tap;
    uint8_t sel_used;
    uint32_t btn_t[NB];                  /* when each button went down */
    uint8_t opened[NB];                  /* its page opened by this press (held long: closes on letting go) */
    uint32_t oct_t, oct_next;
    uint32_t enc_t[NE];                  /* the last turn (acceleration) */
    char ov_name[16], ov_val[24];        /* the value just changed, over the screen for a moment */
    int32_t ov_bar, ov_wave;
    uint32_t ov_until;
    int32_t trail_x[24], trail_y[24];
    uint32_t trail_t;
    int glo_key;                         /* GLO: the key being set (-1 none) */
    int glo_copy;                        /* a key's settings copied (SEL + GLO), -1 none */
    keyp_t clip;
    int seq_page;                        /* SEQ: steps 1-16, 17-32 ... */
    int step_held;                       /* SEQ: the step held (0..63), -1 none */
    uint8_t step_used;                   /* turned or given a gesture while held: no toggle on letting go */
    uint32_t pick_until;                 /* SEL + SEQ: a white key 1..8 picks the pattern until then */
    int slot_sel, slot_act;              /* SLOTS: the slot, the action (LOAD SAVE RENAME) */
    int naming, name_pos;                /* SLOTS: renaming, the letter */
    uint8_t backup_done;
    uint8_t glo_hold, pasted;            /* GLO held on its page with a key copied: pasting, not closing */
    uint8_t redraw;
    uint32_t drawn_at;
} ui;

static const uint8_t WK[NWHITE] = PLAT_WHITE_KEYS, BK[NBLACK] = PLAT_BLACK_KEYS;

/* ------------------------------------------------------------------ text --- */
static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}

static void int_text(int32_t v, char *b, int plus)   /* signed; plus: "+" before a positive */
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    else if (plus && v > 0)
        *b++ = '+';
    while (n)
        *b++ = t[--n];
    *b = 0;
}

static void int_cat(char *b, int32_t v, int plus)
{
    while (*b)
        b++;
    int_text(v, b, plus);
}

static void hz_text(float hz, char *t)   /* "2.00HZ", "440HZ", "1.2K" */
{
    int32_t c = (int32_t)(hz * 100.0f + 0.5f);
    t[0] = 0;
    if (hz >= 1000.0f) {
        int_cat(t, (int32_t)hz / 1000, 0);
        text_cat(t, ".");
        int_cat(t, (int32_t)hz / 100 % 10, 0);
        text_cat(t, "K");
    } else if (hz >= 100.0f) {
        int_cat(t, (int32_t)(hz + 0.5f), 0);
        text_cat(t, "HZ");
    } else {
        int_cat(t, c / 100, 0);
        text_cat(t, ".");
        if (c % 100 < 10)
            text_cat(t, "0");
        int_cat(t, c % 100, 0);
        text_cat(t, "HZ");
    }
}

static int shift(void) { return ui.shift_lock || (ui.btn >> B_SEL & 1u); }

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[48];
    for (i = 0; i < 15u && a && a[i]; i++)
        ui.ov_name[i] = a[i];
    ui.ov_name[i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.ov_val[i] = b[i];
    ui.ov_val[i] = 0;
    ui.ov_bar = -1;
    ui.ov_wave = -1;
    ui.ov_until = plat_ms() + 1000u;
    ui.redraw = 1;
    if (ann_on()) {                      /* and to a screen reader (announce.c, fm1-reader) */
        t[0] = 0;
        text_cat(t, ui.ov_name);
        if (ui.ov_val[0]) {
            text_cat(t, " ");
            text_cat(t, ui.ov_val);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

static void osc_name(int i, char *t)
{
    t[0] = 'O';
    t[1] = 'S';
    t[2] = 'C';
    t[3] = (char)('1' + i);
    t[4] = 0;
}

static void say_tone(int i)
{
    char n[8], v[16];
    osc_name(i, n);
    int_text(patch.semi[i], v, 1);
    if (patch.fine[i]) {
        text_cat(v, " ");
        int_cat(v, patch.fine[i], 1);
        text_cat(v, "C");
    }
    app_say(n, v);
}

static void key_name(int k, char *t)     /* F3 .. G5 */
{
    static const char *const N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int note = 53 + k;
    t[0] = 0;
    text_cat(t, N[note % 12]);
    int_cat(t, note / 12 - 1, 0);
}

/* --------------------------------------------------------------- the rows --- */
/* a page is rows (SELECT), each of up to 5 parameters (PRESET), each with its value (ALGO) */
enum { T_U8, T_S8, T_U16, T_I16 };
enum { F_NUM, F_PCT, F_ONOFF, F_LIST, F_CHZ, F_MS, F_SEMI, F_BIPCT, F_MASK, F_POS, F_LFOHZ, F_CUT, F_RINGHZ,
       F_CHRATE, F_TEXT };
enum { H_NONE, H_ENV, H_SNAP, H_SYS };
typedef struct {
    const char *name;
    void *p;
    uint8_t t, fmt, hook;
    int16_t lo, hi;
    const char *const *list;
} col_t;
typedef struct {
    const char *name;
    int8_t on;                           /* the column that is the row's ON (its dot), -1 none */
    uint8_t n;
    col_t c[5];
} row_t;
static row_t rows[24];
static int nrows;

static const char *const ONOFF[2] = {"OFF", "ON"};
static const char *const VCF_MODE[3] = {"LP", "BP", "HP"};
static const char *const VCA_SHAPE[3] = {"SINE", "SQUARE", "RAMP"};
static const char *const LFO_SHAPE[7] = {"SINE", "TRI", "SAW", "SQUARE", "S&H", "SMOOTH", "CLOCK"};
static const char *const CLK_SRC[3] = {"INT", "MIDI", "OSC4"};
static const char *const REL_MODE[3] = {"FADE", "STAY", "RETURN"};
static const char *const CURVE[3] = {"LINEAR", "SOFT", "EXP"};
static const char *const ARP_MODE[6] = {"UP", "DOWN", "PING-PONG", "RANDOM", "AS PLAYED", "ORBIT"};
static const char *const SEQ_DIR[4] = {"FORWARD", "BACKWARD", "PING-PONG", "RANDOM"};
static const char *const GSPEED[NGSPEED] = {"1/4 BAR", "1/2 BAR", "1 BAR", "2 BARS", "4 BARS", "0.1HZ", "0.25HZ",
                                            "0.5HZ", "1HZ", "2HZ", "4HZ"};
static const char *const GSPEED_K[NGSPEED + 1] = {"EDIT'S", "1/4 BAR", "1/2 BAR", "1 BAR", "2 BARS", "4 BARS",
                                                  "0.1HZ", "0.25HZ", "0.5HZ", "1HZ", "2HZ", "4HZ"};
static const char *const SNAP[2] = {"NONE", "TAKEN"};
static const char *const THEME[3] = {"NEON", "ACIDO", "LAVA"};
static const char *const LEDS[3] = {"ON", "DIM", "KEYS ONLY"};
static const char *const MIDI_CH[17] = {"ALL", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13",
                                        "14", "15", "16"};
static const char *const SLOT_ACT[3] = {"LOAD", "SAVE", "RENAME"};
static const char *const LANE_NAME[NLANE] = {"KAOS", "OSC1", "OSC2", "OSC3", "OSC4"};
static const char *const PAT_NAME[NPAT] = {"1", "2", "3", "4", "5", "6", "7", "8"};

static col_t C(const char *name, void *p, int t, int lo, int hi, int fmt)
{
    col_t c;
    c.name = name;
    c.p = p;
    c.t = (uint8_t)t;
    c.lo = (int16_t)lo;
    c.hi = (int16_t)hi;
    c.fmt = (uint8_t)fmt;
    c.hook = H_NONE;
    c.list = 0;
    return c;
}
static col_t CL(const char *name, void *p, int n, const char *const *list)
{
    col_t c = C(name, p, T_U8, 0, n - 1, F_LIST);
    c.list = list;
    return c;
}
static row_t *R(const char *name)
{
    row_t *r = &rows[nrows++];
    r->name = name;
    r->on = -1;
    r->n = 0;
    return r;
}
static void add(row_t *r, col_t c)
{
    if (r->n < 5u)
        r->c[r->n++] = c;
}
static void add_on(row_t *r, col_t c)
{
    r->on = (int8_t)r->n;
    add(r, c);
}

static int32_t col_get(const col_t *c)
{
    switch (c->t) {
    default:
    case T_U8: return *(uint8_t *)c->p;
    case T_S8: return *(int8_t *)c->p;
    case T_U16: return *(uint16_t *)c->p;
    case T_I16: return *(int16_t *)c->p;
    }
}
static void col_set(const col_t *c, int32_t v)
{
    v = clamp(v, c->lo, c->hi);
    switch (c->t) {
    default:
    case T_U8: *(uint8_t *)c->p = (uint8_t)v; break;
    case T_S8: *(int8_t *)c->p = (int8_t)v; break;
    case T_U16: *(uint16_t *)c->p = (uint16_t)v; break;
    case T_I16: *(int16_t *)c->p = (int16_t)v; break;
    }
}

static void col_text(const col_t *c, char *t)
{
    int32_t v = col_get(c);
    t[0] = 0;
    switch (c->fmt) {
    default:
    case F_NUM: int_text(v, t, 0); break;
    case F_PCT: int_text(v, t, 0); text_cat(t, "%"); break;
    case F_BIPCT: int_text(v, t, 1); text_cat(t, "%"); break;
    case F_ONOFF: text_cat(t, ONOFF[v != 0]); break;
    case F_LIST: text_cat(t, c->list[clamp(v, c->lo, c->hi)]); break;
    case F_CHZ: hz_text((float)v * 0.01f, t); break;
    case F_MS: int_text(v, t, 0); text_cat(t, "MS"); break;
    case F_SEMI: int_text(v, t, 1); break;
    case F_MASK: {
        int i;
        for (i = 0; i < SYN_NOSC; i++)
            t[i] = (v >> i & 1) ? (char)('1' + i) : '-';
        t[SYN_NOSC] = 0;
        break;
    }
    case F_POS:
        if (v == 255)
            text_cat(t, "AUTO");
        else
            int_text(v, t, 0);
        break;
    case F_LFOHZ: hz_text(0.02f * fm_exp2f((float)v * (10.0f / 99.0f)), t); break;
    case F_CUT: hz_text(30.0f * fm_exp2f((float)v * 0.0905f), t); break;
    case F_RINGHZ: hz_text(20.0f * fm_exp2f((float)v * 0.0664f), t); break;
    case F_CHRATE: hz_text(0.05f * fm_exp2f((float)v * 0.0664f), t); break;
    case F_TEXT: text_cat(t, c->list[0]); break;
    }
}

/* each page's rows, made as it is drawn (some depend on the key or the lane chosen) */
static void build(int page)
{
    row_t *r;
    int i;
    nrows = 0;
    switch (page) {
    case P_FX: {
        static const char *const NAME[NFX] = {"VCF", "VCA", "DRIVE", "FOLD", "RING", "CHORUS", "DELAY", "REVERB",
                                              "CRUSH"};
        for (i = 0; i < NFX; i++) {
            fxp_t *f = &patch.fx[i];
            r = R(NAME[i]);
            add_on(r, C("ON", &f->on, T_U8, 0, 1, F_ONOFF));
            switch (i) {
            case FX_VCF:
                add(r, C("CUT", &f->amt, T_U8, 0, 100, F_CUT));
                add(r, C("RESO", &f->a, T_U8, 0, 100, F_PCT));
                add(r, CL("MODE", &f->b, 3, VCF_MODE));
                break;
            case FX_VCA:
                add(r, C("DEPTH", &f->amt, T_U8, 0, 100, F_PCT));
                add(r, CL("RATE", &f->a, NRATE, RATE_NAME));
                add(r, CL("SHAPE", &f->b, 3, VCA_SHAPE));
                break;
            case FX_DRIVE:
                add(r, C("AMT", &f->amt, T_U8, 0, 100, F_PCT));
                add(r, C("TONE", &f->a, T_U8, 0, 100, F_PCT));
                break;
            case FX_FOLD:
                add(r, C("AMT", &f->amt, T_U8, 0, 100, F_PCT));
                add(r, C("SYM", &f->a, T_U8, 0, 100, F_PCT));
                break;
            case FX_RING:
                add(r, C("MIX", &f->amt, T_U8, 0, 100, F_PCT));
                add(r, C("FREQ", &f->a, T_U8, 0, 100, F_RINGHZ));
                break;
            case FX_CHORUS:
                add(r, C("MIX", &f->amt, T_U8, 0, 100, F_PCT));
                add(r, C("RATE", &f->a, T_U8, 0, 100, F_CHRATE));
                add(r, C("DEPTH", &f->b, T_U8, 0, 100, F_PCT));
                break;
            case FX_DELAY:
                add(r, C("MIX", &f->amt, T_U8, 0, 100, F_PCT));
                add(r, CL("TIME", &f->a, NRATE, RATE_NAME));
                add(r, C("FDBK", &f->b, T_U8, 0, 100, F_PCT));
                break;
            case FX_REVERB:
                add(r, C("MIX", &f->amt, T_U8, 0, 100, F_PCT));
                add(r, C("SIZE", &f->a, T_U8, 0, 100, F_PCT));
                add(r, C("DAMP", &f->b, T_U8, 0, 100, F_PCT));
                break;
            case FX_CRUSH:
                add(r, C("BITS", &f->amt, T_U8, 0, 100, F_PCT));
                add(r, C("RATE", &f->a, T_U8, 0, 100, F_PCT));
                break;
            }
        }
        r = R("FX LEVEL");
        add(r, C("DRY/WET", &patch.fx_level, T_U8, 0, 100, F_PCT));
        break;
    }
    case P_ENV: {
        col_t c = CL("ENV", &patch.env, 8, ENV_NAME);
        c.hook = H_ENV;
        r = R("AMBIENT");
        add(r, c);
        add(R("DRIFT"), C("AMOUNT", &patch.drift, T_U8, 0, 100, F_PCT));
        add(R("DRIFT SPEED"), C("SPEED", &patch.drift_speed, T_U8, 0, 100, F_PCT));
        add(R("MUTATION"), C("MUTATION", &patch.mutation, T_U8, 0, 100, F_PCT));
        add(R("DENSITY"), C("DENSITY", &patch.density, T_U8, 0, 100, F_PCT));
        break;
    }
    case P_LFO:
        for (i = 0; i < 2; i++) {
            lfop_t *l = &patch.lfo[i];
            r = R(i ? "LFO 2" : "LFO 1");
            add(r, CL("SHAPE", &l->shape, 7, LFO_SHAPE));
            if (l->sync)
                add(r, CL("RATE", &l->rate, NRATE, RATE_NAME));
            else
                add(r, C("RATE", &l->rate, T_U8, 0, 99, F_LFOHZ));
            add(r, C("SYNC", &l->sync, T_U8, 0, 1, F_ONOFF));
            add(r, CL("DEST", &l->dest, ND, DEST_NAME));
            add(r, C("AMT", &l->amt, T_S8, -100, 100, F_BIPCT));
        }
        r = R("CLOCK");
        add(r, C("RATE", &patch.clock_chz, T_U16, 5, 20000, F_CHZ));
        add(r, CL("SOURCE", &patch.clock_src, 3, CLK_SRC));
        add(r, C("SWING", &patch.clock_swing, T_U8, 0, 75, F_PCT));
        add(r, C("JITTER", &patch.clock_jitter, T_U8, 0, 100, F_PCT));
        break;
    case P_EDIT: {
        static const char *const NAME[4] = {"X DEST 1", "X DEST 2", "Y DEST 1", "Y DEST 2"};
        for (i = 0; i < 4; i++) {
            r = R(NAME[i]);
            add(r, CL("DEST", i < 2 ? &patch.xdest[i] : &patch.ydest[i - 2], ND_PAD, DEST_NAME));
            add(r, C("AMT", i < 2 ? (void *)&patch.xamt[i] : (void *)&patch.yamt[i - 2], T_S8, -100, 100, F_BIPCT));
        }
        add(R("SLEW"), C("SLEW", &patch.slew_ms, T_I16, 0, 2000, F_MS));
        r = R("FADE");
        add(r, C("ATTACK", &patch.attack_ms, T_I16, 0, 2000, F_MS));
        add(r, C("RELEASE", &patch.release_ms, T_I16, 0, 4000, F_MS));
        add(R("RELEASE MODE"), CL("MODE", &patch.rel_mode, 3, REL_MODE));
        add(R("RANGE"), C("RANGE", &patch.range, T_U8, 0, 100, F_PCT));
        add(R("CURVE"), CL("CURVE", &patch.curve, 3, CURVE));
        r = R("GESTURES");
        add(r, C("SIZE", &patch.gsize, T_U8, 1, 100, F_PCT));
        add(r, CL("SPEED", &patch.gspeed, NGSPEED, GSPEED));
        break;
    }
    case P_GLO: {
        keyp_t *k;
        int black = 0;
        if (ui.glo_key < 0)
            break;
        k = &patch.key[ui.glo_key];
        for (i = 0; i < NBLACK; i++)
            black |= BK[i] == ui.glo_key;
        if (black) {
            add(R("GESTURE"), CL("TYPE", &k->gest, NGEST, GEST_NAME));
            r = R("MOTION");
            add(r, CL("SPEED", &k->speed, NGSPEED + 1, GSPEED_K));
            add(r, C("SIZE", &k->size, T_U8, 0, 100, F_PCT));
            r = R("CENTRE");
            add(r, C("X", &k->x, T_U8, 0, 255, F_POS));
            add(r, C("Y", &k->y, T_U8, 0, 255, F_POS));
        } else {
            r = R("POINT");
            add(r, C("X", &k->x, T_U8, 0, 100, F_NUM));
            add(r, C("Y", &k->y, T_U8, 0, 100, F_NUM));
        }
        add(R("INTENSITY"), C("INTENSITY", &k->intens, T_U8, 0, 100, F_PCT));
        add(R("OSCILLATORS"), C("MASK", &k->mask, T_U8, 1, 15, F_MASK));
        r = R("OWN X");
        add(r, CL("DEST", &k->xdest, ND_PAD, DEST_NAME));
        add(r, C("AMT", &k->xamt, T_S8, -100, 100, F_BIPCT));
        r = R("OWN Y");
        add(r, CL("DEST", &k->ydest, ND_PAD, DEST_NAME));
        add(r, C("AMT", &k->yamt, T_S8, -100, 100, F_BIPCT));
        if (!black) {
            col_t c = CL("SNAPSHOT", &k->snap, 2, SNAP);
            c.hook = H_SNAP;
            add(R("SNAPSHOT"), c);
        }
        break;
    }
    case P_ARP:
        add_on(R("ARP"), C("ON", &patch.arp_on, T_U8, 0, 1, F_ONOFF));
        add(R("MODE"), CL("MODE", &patch.arp_mode, 6, ARP_MODE));
        add(R("RATE"), CL("RATE", &patch.arp_rate, NRATE, RATE_NAME));
        add(R("GATE"), C("GATE", &patch.arp_gate, T_U8, 10, 100, F_PCT));
        add(R("GLIDE"), C("GLIDE", &patch.arp_glide, T_U8, 0, 100, F_PCT));
        add(R("SWING"), C("SWING", &patch.arp_swing, T_U8, 0, 75, F_PCT));
        add(R("LATCH"), C("LATCH", &patch.arp_latch, T_U8, 0, 1, F_ONOFF));
        add(R("SPREAD"), C("SPREAD", &patch.arp_spread, T_U8, 0, 100, F_PCT));
        add(R("CLOCK"), C("RATE", &patch.clock_chz, T_U16, 5, 20000, F_CHZ));
        break;
    case P_SEQ: {
        pattern_t *pt = &pats[patch.pat & 7];
        int l = eng_lane;
        add(R("PATTERN"), CL("PATTERN", &patch.pat, NPAT, PAT_NAME));
        r = R("LANE");
        add(r, C("LENGTH", &pt->len[l], T_U8, 1, NSTEP, F_NUM));
        add(r, C("MUTATION", &pt->mut[l], T_U8, 0, 100, F_PCT));
        add(R("RATE"), CL("RATE", &patch.seq_rate, NRATE, RATE_NAME));
        add(R("SWING"), C("SWING", &patch.seq_swing, T_U8, 0, 75, F_PCT));
        add(R("DIRECTION"), CL("DIR", &patch.seq_dir, 4, SEQ_DIR));
        add(R("CLOCK"), C("RATE", &patch.clock_chz, T_U16, 5, 20000, F_CHZ));
        break;
    }
    case P_SYS: {
        static const char *const VER[1] = {MD_VERSION};
        col_t c;
        c = CL("CHANNEL", &sys.midi_ch, 17, MIDI_CH);
        c.hook = H_SYS;
        add(R("MIDI CHANNEL"), c);
        c = C("NOTES", &sys.midi_notes, T_U8, 0, 1, F_ONOFF);
        c.hook = H_SYS;
        add(R("MIDI NOTES"), c);
        c = C("CC OUT", &sys.cc_out, T_U8, 0, 1, F_ONOFF);
        c.hook = H_SYS;
        add(R("CC OUT"), c);
        c = CL("THEME", &sys.theme, 3, THEME);
        c.hook = H_SYS;
        add(R("PAD COLOURS"), c);
        c = CL("LEDS", &sys.leds, 3, LEDS);
        c.hook = H_SYS;
        add(R("LIGHTS"), c);
        c = C("VERSION", &sys.rsv[0], T_U8, 0, 0, F_TEXT);
        c.list = VER;
        add(R("VERSION"), c);
        {   /* the audio's load and its dropouts: how much room the sound has */
            static char load[24];
            static const char *const LOAD[1] = {load};
            load[0] = 0;
            int_cat(load, (int32_t)plat_cpu_pct(), 0);
            text_cat(load, "%  XRUN ");
            int_cat(load, (int32_t)plat_xruns(), 0);
            c = C("AUDIO", &sys.rsv[0], T_U8, 0, 0, F_TEXT);
            c.list = LOAD;
            add(R("AUDIO LOAD"), c);
        }
        break;
    }
    default:
        break;
    }
}

static int col_in_patch(const col_t *c)
{
    return (uint8_t *)c->p >= (uint8_t *)&patch && (uint8_t *)c->p < (uint8_t *)&patch + sizeof patch;
}

static void say_col(const row_t *r, const col_t *c)
{
    char t[24];
    col_text(c, t);
    app_say(r->n > 1 ? c->name : r->name, t);
    if (ui.page >= P_FX && ui.page != P_SEQ) {
        ui.ov_until = 0;                 /* (the value is on the page already: said, not shown) */
        return;
    }
    if (c->fmt == F_PCT || c->fmt == F_CUT || c->fmt == F_RINGHZ || c->fmt == F_CHRATE)
        ui.ov_bar = (int32_t)(col_get(c) - c->lo) * 1000 / (c->hi - c->lo ? c->hi - c->lo : 1);
}

static void col_turn(const row_t *r, const col_t *c, int32_t d)
{
    int32_t v = col_get(c), nv;
    if (c->fmt == F_TEXT)
        return;
    if (c->fmt == F_POS && v == 255)     /* AUTO: turning gives a place */
        v = d > 0 ? -1 : 101;
    nv = v + d;
    if (c->fmt == F_POS) {
        if (nv > 100 || nv < 0)
            nv = 255;
    } else if (c->fmt == F_CHZ) {        /* the clock: finer at the bottom */
        int32_t s = v < 100 ? 1 : v < 1000 ? 5 : 50;
        nv = v + d * s;
    }
    col_set(c, nv);
    if (col_in_patch(c))
        patch_dirty = 1;
    switch (c->hook) {
    case H_ENV:
        env_apply(col_get(c));
        break;
    case H_SNAP:
        if (col_get(c)) {
            keyp_t *k = &patch.key[ui.glo_key];
            int i;
            for (i = 0; i < SYN_NOSC; i++)
                k->snap_vol[i] = (uint8_t)vol[i];
            k->snap_algo = patch.algo;
        }
        break;
    case H_SYS:
        sys_save();
        break;
    default:
        break;
    }
    say_col(r, c);
}

/* ------------------------------------------------------------------ actions --- */
static void tone_step(int32_t d)
{
    int i = patch.sel;
    patch.semi[i] = (int8_t)clamp(patch.semi[i] + d, -48, 48);
    patch_dirty = 1;
    say_tone(i);
}

static void tone_reset(void)
{
    int i;
    for (i = 0; i < SYN_NOSC; i++)
        if (shift() || i == patch.sel) {
            patch.semi[i] = 0;
            patch.fine[i] = 0;
        }
    patch_dirty = 1;
    if (shift())
        app_say("TONE", "ALL 0");
    else
        say_tone(patch.sel);
}

static void slot_say(int i, const char *what)
{
    char t[24], n[NAME_LEN + 1];
    t[0] = 0;
    int_cat(t, i + 1, 0);
    text_cat(t, " ");
    slot_name(i, n);
    text_cat(t, n);
    app_say(what, t);
}

static void do_save(int slot)
{
    if (slot_save(slot) == 0) {
        patch_dirty = 0;
        if (slot < NSLOT && sys.slot != slot) {
            sys.slot = (uint8_t)slot;
            sys_save();
        }
        slot_say(slot, slot == NSLOT ? "BACKUP" : "SAVED");
    } else {
        app_say("SAVE", "FAILED");
    }
}

static void do_load(int slot)
{
    if (slot_load(slot) == 0) {
        patch_dirty = 0;
        sys.slot = (uint8_t)slot;
        sys_save();
        slot_say(slot, "LOADED");
    } else {
        slot_say(slot, "EMPTY");
    }
}

static uint32_t ui_rnd(void)
{
    static uint32_t s = 0x6D2B79F5u;
    s ^= plat_ms();
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static uint8_t nudge(uint8_t v, int32_t r)
{
    return (uint8_t)clamp((int32_t)v + (int32_t)(ui_rnd() % (uint32_t)(2 * r + 1)) - r, 0, 100);
}

static void mutate(void)                 /* SEL + ENV: the environment a little different */
{
    int i;
    patch.drift = nudge(patch.drift, 12);
    patch.mutation = nudge(patch.mutation, 10);
    patch.density = nudge(patch.density, 10);
    for (i = 0; i < NFX; i++)
        if (patch.fx[i].on)
            patch.fx[i].amt = nudge(patch.fx[i].amt, 8);
    eng_cmd(C_RESEED, (int)(ui_rnd() & 0xFFFFu));
    patch_dirty = 1;
    app_say("MUTATE", ENV_NAME[patch.env & 7]);
}

static void page_open(int p)
{
    if (p == ui.page)
        return;
    ui.page = p;
    ui.step_held = -1;
    ui.naming = 0;
    if (p == P_KAOS || p == P_BARS)
        ui.main = p;
    lcd_fill(0, 0, 240, 240, p == P_KAOS ? C_BLACK : RGB(16, 16, 16));
    ui.redraw = 1;
}

static void page_close(void) { page_open(ui.main); }

static int page_of_button(uint32_t b)
{
    switch (b) {
    case B_FX: return P_FX;
    case B_ENV: return P_ENV;
    case B_LFO: return P_LFO;
    case B_EDIT: return P_EDIT;
    case B_GLO: return P_GLO;
    case B_ARP: return P_ARP;
    case B_SEQ: return P_SEQ;
    default: return -1;
    }
}

static void button(uint32_t b)
{
    uint32_t both = 1u << B_OCTDN | 1u << B_OCTUP, now = plat_ms();
    int pg = page_of_button(b);
    ui.btn_t[b] = now;
    ui.opened[b] = 0;
    if (b == B_SEL) {                    /* SHIFT: held; two clean taps lock it, a tap lets it go */
        if (ui.shift_lock)
            ui.shift_lock = 0;
        else if (ui.sel_tap && now - ui.sel_tap < 300u)
            ui.shift_lock = 1;
        ui.sel_down = now;
        ui.sel_used = 0;
        ui.sel_tap = 0;
        return;
    }
    ui.sel_used = 1;
    ui.redraw = 1;
    if (shift()) {
        switch (b) {
        case B_FX:
            fx_bypass = !fx_bypass;
            app_say("FX", fx_bypass ? "BYPASS" : "ON");
            return;
        case B_ENV:
            mutate();
            return;
        case B_LFO:
            patch.lfo[0].sync = patch.lfo[1].sync = (uint8_t)!patch.lfo[0].sync;
            patch.lfo[0].rate = patch.lfo[0].sync ? 2 : 40;
            patch.lfo[1].rate = patch.lfo[1].sync ? 3 : 50;
            patch_dirty = 1;
            app_say("LFO SYNC", ONOFF[patch.lfo[0].sync]);
            return;
        case B_EDIT:
            env_edit_defaults(patch.env);
            patch_dirty = 1;
            app_say("EDIT", "DEFAULTS");
            return;
        case B_GLO:
            if (ui.glo_key >= 0) {
                ui.clip = patch.key[ui.glo_key];
                ui.glo_copy = ui.glo_key;
                app_say("KEY", "COPIED");
            } else {
                app_say("GLO", "PICK A KEY");
            }
            return;
        case B_HOME:
            page_open(ui.page == P_SYS ? ui.main : P_SYS);
            return;
        case B_SAVE:
            ui.backup_done = 0;
            ui.slot_sel = sys.slot;
            page_open(ui.page == P_SAVE ? ui.main : P_SAVE);
            return;
        case B_ARP:
            patch.arp_on = (uint8_t)!patch.arp_on;
            patch_dirty = 1;
            app_say("ARP", ONOFF[patch.arp_on]);
            return;
        case B_SEQ:
            ui.pick_until = now + 3000u;
            app_say("PATTERN", "WHITE KEY 1-8");
            return;
        case B_PLAY:
            if (ui.btn >> B_REC & 1u) {  /* SEL + REC + PLAY: the whole pattern */
                pattern_defaults(&pats[patch.pat & 7]);
                patch_dirty = 1;
                app_say("PATTERN", "CLEARED");
            } else {
                eng_cmd(C_TAP, 0);
                app_say("TAP", "");
            }
            return;
        case B_REC: {
            pattern_t *pt = &pats[patch.pat & 7];
            int s, l = eng_lane;
            for (s = 0; s < NSTEP; s++) {
                if (l == 0)
                    pt->k[s].f = 0;
                else
                    pt->o[l - 1][s].f = 0;
            }
            patch_dirty = 1;
            app_say("LANE CLEARED", LANE_NAME[l]);
            return;
        }
        default:
            break;
        }
    }
    switch (b) {
    case B_HOME:
        if (ui.page == P_KAOS || ui.page == P_BARS)
            page_open(ui.page == P_KAOS ? P_BARS : P_KAOS);
        else
            page_close();
        break;
    case B_SAVE:
        if (ui.page == P_SAVE) {         /* the slots: SAVE does the action */
            if (ui.naming) {
                ui.naming = 0;
                patch_dirty = 1;
                app_say("NAME", "SET");
            } else if (ui.slot_act == 0) {
                do_load(ui.slot_sel);
            } else if (ui.slot_act == 1) {
                do_save(ui.slot_sel);
            } else {
                ui.naming = 1;
                ui.name_pos = 0;
            }
        } else {
            do_save(sys.slot);
        }
        break;
    case B_PLAY:
        eng_cmd(C_PLAY, 0);
        break;
    case B_REC:
        eng_cmd(C_REC, 0);
        break;
    case B_OCTDN:
    case B_OCTUP:
        if ((plat_buttons() & both) == both) {
            tone_reset();
            ui.oct_t = 0;
        } else {
            tone_step((b == B_OCTUP ? 1 : -1) * (shift() ? 12 : 1));
            ui.oct_t = now;
            ui.oct_next = now + 400u;
        }
        break;
    default:
        if (pg >= 0) {
            if (pg == P_GLO && ui.page == P_GLO && ui.glo_copy >= 0) {
                ui.glo_hold = 1;         /* GLO + keys: paste; GLO alone: closes on letting go */
                ui.pasted = 0;
            } else if (ui.page == pg) {
                page_close();
            } else {
                page_open(pg);
                ui.opened[b] = 1;
            }
        }
        break;
    }
}

static void button_up(uint32_t b)
{
    uint32_t now = plat_ms();
    if (b == B_SEL) {                    /* SEL let go: a clean tap, or SHIFT used */
        ui.sel_tap = !ui.sel_used && now - ui.sel_down < 250u && !ui.shift_lock ? now : 0;
        return;
    }
    if (b == B_GLO && ui.glo_hold) {
        ui.glo_hold = 0;
        if (!ui.pasted)
            page_close();
        return;
    }
    if (ui.opened[b] && now - ui.btn_t[b] > 500u && page_of_button(b) == ui.page)
        page_close();                    /* held long: the page was a glance */
    ui.opened[b] = 0;
}

static int32_t accel(uint32_t e, int32_t d)
{
    uint32_t now = plat_ms(), gap = now - ui.enc_t[e];
    ui.enc_t[e] = now;
    return gap < 35u ? d * 5 : d;
}

static void encoder(uint32_t e, int32_t d)
{
    char t[24];
    int i = patch.sel;
    ui.sel_used = 1;
    if (e >= EN_K1) {                    /* K1..K4: the volumes, on every page */
        i = (int)(e - EN_K1);
        if (shift()) {
            patch.fine[i] = (int8_t)clamp(patch.fine[i] + d, -50, 50);
            patch_dirty = 1;
            say_tone(i);
        } else {
            char v[8];
            vol[i] = clamp(vol[i] + accel(e, d) * 2, 0, 100);
            osc_name(i, t);
            int_text(vol[i], v, 0);
            app_say(t, v);
            ui.ov_bar = vol[i] * 10;
        }
        return;
    }
    if (ui.page == P_SAVE) {             /* the slots */
        if (ui.naming) {
            static const char CH[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";
            if (e == EN_SELECT) {
                ui.name_pos = clamp(ui.name_pos + d, 0, NAME_LEN - 1);
            } else if (e == EN_ALGO) {
                int k = 0, n = (int)sizeof CH - 1;
                while (k < n && CH[k] != patch.name[ui.name_pos])
                    k++;
                patch.name[ui.name_pos] = CH[((k + d) % n + n) % n];
                patch_dirty = 1;
            }
            ui.redraw = 1;
            return;
        }
        if (e == EN_SELECT) {
            ui.slot_sel = clamp(ui.slot_sel + d, 0, NSLOT - 1);
            slot_say(ui.slot_sel, "SLOT");
        } else {
            ui.slot_act = clamp(ui.slot_act + d, 0, 2);
            app_say("ACTION", SLOT_ACT[ui.slot_act]);
        }
        ui.ov_until = 0;
        return;
    }
    if (ui.page == P_SEQ && ui.step_held >= 0) {   /* a step held: PRESET its Y (or tone), ALGO its X (or mute) */
        pattern_t *pt = &pats[patch.pat & 7];
        int s = ui.step_held, l = eng_lane;
        char n[12] = "STEP ";
        ui.step_used = 1;
        patch_dirty = 1;
        if (l == 0) {
            kstep_t *k = &pt->k[s];
            if (e == EN_PRESET)
                k->y = (uint8_t)clamp(k->y + accel(e, d), 0, 100);
            else if (e == EN_ALGO)
                k->x = (uint8_t)clamp(k->x + accel(e, d), 0, 100);
            k->f = (uint8_t)((k->f | KS_TOUCH) & ~KS_SUB);
            t[0] = 0;
            text_cat(t, "X ");
            int_cat(t, k->x, 0);
            text_cat(t, " Y ");
            int_cat(t, k->y, 0);
        } else {
            ostep_t *o = &pt->o[l - 1][s];
            if (e == EN_PRESET)
                o->semi = (int8_t)clamp(o->semi + d, -24, 24);
            else if (e == EN_ALGO)
                o->f ^= OS_MUTE;
            o->f |= OS_ON;
            int_text(o->semi, t, 1);
            if (o->f & OS_MUTE)
                text_cat(t, " MUTE");
        }
        int_cat(n, s + 1, 0);
        app_say(n, t);
        return;
    }
    if (ui.page >= P_FX) {               /* a page of rows */
        build(ui.page);
        if (!nrows) {
            app_say("GLO", "PRESS A KEY");
            return;
        }
        ui.row[ui.page] = clamp(ui.row[ui.page], 0, nrows - 1);
        if (e == EN_SELECT) {
            ui.row[ui.page] = clamp(ui.row[ui.page] + d, 0, nrows - 1);
            ui.col[ui.page] = 0;
            ui.redraw = 1;
            if (ann_on())
                ann_say(rows[ui.row[ui.page]].name, 0, ANN_NOW);
        } else {
            row_t *r = &rows[ui.row[ui.page]];
            int c = clamp(ui.col[ui.page], 0, r->n - 1);
            if (e == EN_PRESET) {
                ui.col[ui.page] = clamp(c + d, 0, r->n - 1);
                say_col(r, &r->c[ui.col[ui.page]]);
            } else {
                int32_t dd = r->c[c].hi - r->c[c].lo > 30 ? accel(e, d) : d;
                col_turn(r, &r->c[c], dd);
            }
        }
        return;
    }
    /* the main pages */
    switch (e) {
    case EN_SELECT:
        if (shift()) {
            int32_t v = patch.clock_chz, s = v < 100 ? 1 : v < 1000 ? 5 : 50;
            patch.clock_chz = (uint16_t)clamp(v + accel(e, d) * s, 5, 20000);
            patch_dirty = 1;
            hz_text((float)patch.clock_chz * 0.01f, t);
            text_cat(t, " ");
            int_cat(t, patch.clock_chz * 60 / 100, 0);
            text_cat(t, "BPM");
            app_say("CLOCK", t);
            break;
        }
        patch.sel = (uint8_t)clamp(patch.sel + d, 0, SYN_NOSC - 1);
        osc_name(patch.sel, t);
        text_cat(t, " ");
        text_cat(t, SYN_WAVE_SHORT[patch.wave[patch.sel]]);
        app_say("ACTIVE", t);
        break;
    case EN_PRESET:
        if (shift()) {                   /* the next or the last patch (not the one in use again) */
            int n = clamp(sys.slot + d, 0, NSLOT - 1);
            if (n != sys.slot) {
                ui.slot_sel = n;
                do_load(n);
            }
            break;
        }
        patch.wave[i] = (int8_t)(((patch.wave[i] + d) % SYN_NWAVES + SYN_NWAVES) % SYN_NWAVES);
        patch_dirty = 1;
        osc_name(i, t);
        app_say(t, SYN_WAVE_NAME[patch.wave[i]]);
        ui.ov_wave = patch.wave[i];
        break;
    case EN_ALGO:
        if (shift()) {
            patch.fx_level = (uint8_t)clamp(patch.fx_level + accel(e, d) * 2, 0, 100);
            patch_dirty = 1;
            int_text(patch.fx_level, t, 0);
            text_cat(t, "%");
            app_say("FX DRY/WET", t);
            ui.ov_bar = patch.fx_level * 10;
            break;
        }
        patch.algo = (uint8_t)clamp(patch.algo + d, 0, SYN_NALGOS - 1);
        patch_dirty = 1;
        int_text(patch.algo + 1, t, 0);
        text_cat(t, " ");
        text_cat(t, SYN_ALGO_NAME[patch.algo]);
        app_say("ALGO", t);
        break;
    default:
        break;
    }
}

/* the keys: the pad, the steps (SEQ), the key to set (GLO), step entry (REC while stopped), a pattern (SEL+SEQ) */
static void keys_in(uint32_t keys)
{
    uint32_t down = keys & ~ui.keys, up = ui.keys & ~keys, k, now = plat_ms();
    int i, step_entry = eng.rec && !eng.playing;
    if (keys != ui.keys) {
        ui.redraw = 1;
        ui.sel_used = 1;
    }
    if (now < ui.pick_until) {           /* SEL + SEQ, then a white key: the pattern */
        for (i = 0; i < NPAT; i++)
            if (down >> WK[i] & 1u) {
                patch.pat = (uint8_t)i;
                patch_dirty = 1;
                ui.pick_until = 0;
                app_say("PATTERN", PAT_NAME[i]);
                pad_keys = 0;
                return;
            }
    }
    if (ui.page == P_GLO) {              /* a key to set; GLO held: paste the copied one onto it */
        pad_keys = 0;
        for (k = 0; k < NKEYS; k++)
            if (down >> k & 1u) {
                char t[8];
                key_name((int)k, t);
                if (ui.glo_hold && ui.glo_copy >= 0) {
                    patch.key[k] = ui.clip;
                    patch_dirty = 1;
                    ui.pasted = 1;
                    app_say("PASTED", t);
                } else {
                    app_say("KEY", t);
                }
                ui.glo_key = (int)k;
                ui.row[P_GLO] = 0;
                ui.col[P_GLO] = 0;
            }
        return;
    }
    if (ui.page == P_SEQ) {              /* the steps */
        pattern_t *pt = &pats[patch.pat & 7];
        pad_keys = 0;
        for (i = 0; i < NWHITE; i++) {
            int s = ui.seq_page * 16 + i;
            if (down >> WK[i] & 1u) {
                ui.step_held = s;
                ui.step_used = 0;
            } else if ((up >> WK[i] & 1u) && ui.step_held == s) {
                if (!ui.step_used) {     /* a tap: the step on or off */
                    if (eng_lane == 0) {
                        kstep_t *ks = &pt->k[s];
                        if (!(ks->f & KS_TOUCH)) {
                            ks->x = (uint8_t)(eng.cx / 10);
                            ks->y = (uint8_t)(eng.cy / 10);
                            ks->f &= (uint8_t)~KS_SUB;
                        }
                        ks->f ^= KS_TOUCH;
                    } else {
                        pt->o[eng_lane - 1][s].f ^= OS_ON;
                    }
                    patch_dirty = 1;
                }
                ui.step_held = -1;
            }
        }
        for (i = 0; i < NBLACK; i++)
            if (down >> BK[i] & 1u) {
                if (ui.step_held >= 0 && eng_lane == 0) {   /* a step held: its gesture */
                    kstep_t *ks = &pt->k[ui.step_held];
                    int g = patch.key[BK[i]].gest;
                    ks->g = (uint8_t)(ks->g == g + 1 ? 0 : g + 1);
                    ks->f |= KS_TOUCH;
                    ui.step_used = 1;
                    patch_dirty = 1;
                    app_say("STEP GESTURE", ks->g ? GEST_NAME[g] : "NONE");
                } else if (i < 4) {
                    char t[12] = "";
                    ui.seq_page = i;
                    int_cat(t, i * 16 + 1, 0);
                    text_cat(t, "-");
                    int_cat(t, i * 16 + 16, 0);
                    app_say("STEPS", t);
                } else if (i < 9) {
                    eng_lane = i - 4;
                    app_say("LANE", LANE_NAME[eng_lane]);
                } else if (i == 9) {     /* the page onto the next one */
                    int n = (ui.seq_page + 1) % 4, s;
                    for (s = 0; s < 16; s++) {
                        pt->k[n * 16 + s] = pt->k[ui.seq_page * 16 + s];
                        for (k = 0; k < SYN_NOSC; k++)
                            pt->o[k][n * 16 + s] = pt->o[k][ui.seq_page * 16 + s];
                    }
                    ui.seq_page = n;
                    patch_dirty = 1;
                    app_say("PAGE", "COPIED");
                } else {                 /* the page cleared */
                    int s;
                    for (s = 0; s < 16; s++) {
                        if (eng_lane == 0)
                            pt->k[ui.seq_page * 16 + s].f = 0;
                        else
                            pt->o[eng_lane - 1][ui.seq_page * 16 + s].f = 0;
                    }
                    patch_dirty = 1;
                    app_say("PAGE", "CLEARED");
                }
            }
        return;
    }
    if (step_entry) {                    /* REC while stopped: each key fills a step and moves on */
        pad_keys = 0;
        for (i = 0; i < NWHITE; i++)
            if (down >> WK[i] & 1u) {
                eng_cmd(C_STEP_KEY, i);
                patch_dirty = 1;
            }
        for (i = 0; i < NBLACK; i++)
            if (down >> BK[i] & 1u) {
                eng_cmd(C_STEP_GEST, i);
                patch_dirty = 1;
            }
        return;
    }
    if (shift()) {                       /* SEL + white: HOLD; SEL + black: LATCH; SEL + F#5: all stop */
        for (i = 0; i < NWHITE; i++)
            if (down >> WK[i] & 1u) {
                eng_cmd(C_HOLD, i);
                app_say("HOLD", eng.hold == i ? "OFF" : "ON");
            }
        for (i = 0; i < NBLACK; i++)
            if (down >> BK[i] & 1u) {
                if (i == NBLACK - 1) {
                    eng_cmd(C_STOP_GEST, 0);
                    app_say("GESTURES", "STOPPED");
                } else {
                    eng_cmd(C_LATCH, i);
                    app_say(GEST_NAME[patch.key[BK[i]].gest], (eng.latched >> i & 1u) ? "UNLATCHED" : "LATCHED");
                }
            }
        pad_keys = keys & ~down & pad_keys;   /* (keys held from before keep playing) */
        return;
    }
    pad_keys = keys;
}

static void input(void)
{
    uint32_t keys = plat_keys(), btn = plat_buttons(), k, now = plat_ms();
    int32_t e;
    for (k = 0; k < NB; k++) {
        if ((btn >> k & 1u) && !(ui.btn >> k & 1u)) {
            ui.btn = btn & ~(1u << k);   /* (button() sees the others as held) */
            button(k);
        } else if (!(btn >> k & 1u) && (ui.btn >> k & 1u)) {
            ui.btn = btn | (1u << k);
            button_up(k);
        }
    }
    ui.btn = btn;
    if (ui.oct_t && (btn & (1u << B_OCTDN | 1u << B_OCTUP)) && now >= ui.oct_next) {   /* held: on and on */
        tone_step((btn >> B_OCTUP & 1u ? 1 : -1) * (shift() ? 12 : 1));
        ui.oct_next = now + 100u;
    } else if (!(btn & (1u << B_OCTDN | 1u << B_OCTUP))) {
        ui.oct_t = 0;
    }
    if (shift() && (btn >> B_SAVE & 1u) && !ui.backup_done && now - ui.btn_t[B_SAVE] > 2000u) {
        ui.backup_done = 1;              /* SEL + SAVE held 2 s: the quick backup */
        do_save(NSLOT);
        page_close();
    }
    for (k = 0; k < NE; k++)
        if ((e = plat_enc((int)k)) != 0)
            encoder(k, e);
    keys_in(keys);
    ui.keys = keys;
}

/* ------------------------------------------------------------------- lights --- */
static uint32_t nearest_key(void)        /* the white key whose point is nearest the cursor */
{
    int32_t i, best = 0, bd = 0x7FFFFFFF;
    for (i = 0; i < NWHITE; i++) {
        int32_t dx = eng.cx / 10 - patch.key[WK[i]].x, dy = eng.cy / 10 - patch.key[WK[i]].y, d = dx * dx + dy * dy;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return WK[best];
}

static void leds(void)
{
    uint32_t b = 0, db = 0, k = 0, dk = 0, now = plat_ms(), i, beat = eng.pulses;
    static const uint8_t PAGE_BTN[NPAGE] = {0, 0, B_FX, B_ENV, B_LFO, B_EDIT, B_GLO, B_ARP, B_SEQ, B_SAVE, B_HOME};
    if (ui.shift_lock ? (now / 250u) & 1u : ui.btn >> B_SEL & 1u)
        b |= 1u << B_SEL;
    if (ui.page >= P_FX)
        b |= 1u << PAGE_BTN[ui.page];
    if (patch_dirty)
        b |= 1u << B_SAVE;
    for (i = 0; i < NFX; i++)
        if (patch.fx[i].on && !fx_bypass)
            db |= 1u << B_FX;
    if (patch.drift && (now / 700u) & 1u)    /* ENV breathes while the drift is on */
        db |= 1u << B_ENV;
    if (eng.lfo[0] > 0.0f && patch.lfo[0].dest)
        db |= 1u << B_LFO;
    if (patch.arp_on) {
        if (eng.arp_pt >= 0 && (beat & 1u))
            b |= 1u << B_ARP;
        else
            db |= 1u << B_ARP;
    }
    if (eng.playing) {
        b |= 1u << B_SEQ;
        if (beat % 4u == 0)
            b |= LED_PLAY_GREEN;
        else
            db |= LED_PLAY_GREEN;
    }
    if (eng.rec)
        b |= 1u << B_REC;
    plat_play_red(eng.rec && eng.playing);
    if (ui.page == P_SEQ) {              /* the steps with something in them, and the playhead */
        pattern_t *pt = &pats[patch.pat & 7];
        int l = eng_lane;
        for (i = 0; i < NWHITE; i++) {
            int s = ui.seq_page * 16 + (int)i;
            if (l == 0 ? (pt->k[s].f & KS_TOUCH) : (pt->o[l - 1][s].f & OS_ON))
                dk |= 1u << WK[i];
            if ((eng.playing || eng.rec) && eng.pos[l] == s)
                k |= 1u << WK[i];
        }
        k |= 1u << BK[ui.seq_page] | 1u << BK[4 + l];
    } else if (ui.page == P_GLO) {
        if (ui.glo_key >= 0)
            k |= 1u << ui.glo_key;
    } else {
        k = ui.keys | 1u << nearest_key();
        if (eng.arp_pt >= 0)
            k |= 1u << WK[eng.arp_pt];
        for (i = 0; i < NBLACK; i++)
            if (eng.gest_on >> i & 1u)
                k |= 1u << BK[i];
        if (eng.hold >= 0)
            k |= 1u << WK[eng.hold];
    }
    if (sys.leds == 2)
        b = db = 0;
    plat_lights(b, k, sys.leds ? 0 : db, dk, sys.leds ? 0 : 1);
}

void app_input_only(void) { input(); }

/* ------------------------------------------------------------------ screen --- */
#define N_GRID RGB(43, 36, 64)
#define N_TXT RGB(153, 153, 153)
#define A_BG RGB(16, 16, 16)
#define A_INK RGB(232, 232, 232)
#define A_GRAY RGB(138, 138, 138)
#define A_LINE RGB(58, 58, 58)
static const uint16_t THEME_A[3] = {RGB(255, 46, 136), RGB(230, 255, 0), RGB(255, 90, 31)};
static const uint16_t THEME_B[3] = {RGB(0, 229, 255), RGB(122, 60, 255), RGB(255, 23, 68)};
#define N_MAG THEME_A[sys.theme % 3u]
#define N_CYAN THEME_B[sys.theme % 3u]

static uint16_t mix565(uint16_t a, uint16_t b, int32_t t)   /* a .. b, t 0..256 */
{
    int32_t r = (a >> 11) + (((b >> 11) - (a >> 11)) * t >> 8);
    int32_t g = ((a >> 5) & 63) + ((((b >> 5) & 63) - ((a >> 5) & 63)) * t >> 8);
    int32_t bl = (a & 31) + (((b & 31) - (a & 31)) * t >> 8);
    return (uint16_t)(r << 11 | g << 5 | bl);
}

static void g_strip(int32_t y0, int32_t h, uint16_t bg)   /* rows y0 .. y0+h-1, drawn in screen coordinates */
{
    cv_begin(240, (uint32_t)h, bg);
    cv_oy = -y0;
}

static void g_disc(int32_t x, int32_t y, int32_t r, uint16_t c)
{
    int32_t dy, dx;
    for (dy = -r; dy <= r; dy++) {
        for (dx = 0; dx * dx + dy * dy <= r * r; dx++)
            ;
        cv_rect(x - dx + 1, y + dy, 2 * dx - 1, 1, c);
    }
}

static void g_ring(int32_t x, int32_t y, int32_t r, uint16_t c)
{
    int32_t dy, dx;
    for (dy = -r; dy <= r; dy++)
        for (dx = -r; dx <= r; dx++) {
            int32_t d = dx * dx + dy * dy;
            if (d <= r * r && d > (r - 1) * (r - 1))
                cv_pset(x + dx, y + dy, c);
        }
}

static void g_pill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)   /* rounded corners */
{
    cv_rect(x + 2, y, w - 4, h, c);
    cv_rect(x + 1, y + 1, w - 2, h - 2, c);
    cv_rect(x, y + 2, w, h - 4, c);
}

static void g_outline(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c, int dashed)
{
    int32_t i;
    for (i = 0; i < w; i++)
        if (!dashed || (i / 3) % 2 == 0) {
            cv_pset(x + i, y, c);
            cv_pset(x + i, y + h - 1, c);
        }
    for (i = 0; i < h; i++)
        if (!dashed || (i / 3) % 2 == 0) {
            cv_pset(x, y + i, c);
            cv_pset(x + w - 1, y + i, c);
        }
}

static void g_wave(int32_t x, int32_t y, int32_t w, int32_t h, int32_t wave, float amp, uint16_t c)
{
    int32_t i, py = 0;
    for (i = 0; i < w; i++) {
        float v = syn_wave_preview(wave, 2.0f * (float)i / (float)w, 0.35f);   /* two cycles */
        int32_t yy = y + h / 2 - (int32_t)(v * amp * (float)(h / 2 - 1));
        if (i)
            cv_line(x + i - 1, py, x + i, yy, c);
        py = yy;
    }
}

#define PX0 14
#define PX1 226
#define PY0 34
#define PY1 202
static int32_t pad_x(int32_t v) { return PX0 + v * (PX1 - PX0) / 1000; }
static int32_t pad_y(int32_t v) { return PY1 - v * (PY1 - PY0) / 1000; }

static void g_overlay(uint16_t ink, uint16_t dim, uint16_t bg)   /* the value just changed, at the top */
{
    int32_t x;
    cv_rect(0, 0, 240, 58, bg);
    cv_text(12, 4, &FONT_S, ui.ov_name, dim);
    x = cv_text(12, 20, &FONT_L, ui.ov_val[0] ? ui.ov_val : " ", ink);
    if (ui.ov_wave >= 0)
        g_wave(x + 12 > 160 ? 160 : x + 12, 20, 66, 30, ui.ov_wave, 0.9f, ink);
    if (ui.ov_bar >= 0) {
        cv_rect(12, 54, 216, 2, A_LINE);
        cv_rect(12, 54, 216 * clamp(ui.ov_bar, 0, 1000) / 1000, 2, ink);
    }
}

static void dest_value(int d, char *t)   /* a destination's name and value now, for the pad's footer */
{
    t[0] = 0;
    text_cat(t, DEST_NAME[d]);
    if (d) {
        text_cat(t, " ");
        if (d == D_CUTOFF) {
            char h[12];
            hz_text((float)eng.cutoff_hz, h);
            text_cat(t, h);
        } else {
            int_cat(t, (int32_t)(eng.p[d] * 100.0f + 0.5f), 0);
            text_cat(t, "%");
        }
    }
}

static void draw_kaos(void)
{
    char t[32];
    int32_t y0, i, c, r, cx = pad_x(eng.cx), cy = pad_y(eng.cy), amt = eng.amt;
    uint32_t now = plat_ms();
    for (y0 = 0; y0 < 240; y0 += 120) {
        g_strip(y0, 120, C_BLACK);
        /* the header: the clock's dot (lit on every other pulse) and rate, the environment, PLAY / REC */
        g_disc(10, 12, 4, eng.pulses % 2u ? mix565(N_GRID, N_MAG, 220) : N_GRID);
        hz_text(eng.clock_hz, t);
        c = cv_text(20, 4, &FONT_S, t, N_TXT);
        cv_text(c + 8, 4, &FONT_S, ENV_NAME[patch.env & 7], N_CYAN);
        t[0] = 0;
        if (eng.rec)
            text_cat(t, "REC ");
        if (eng.playing) {
            text_cat(t, "P");
            int_cat(t, patch.pat + 1, 0);
        }
        if (fx_bypass)
            text_cat(t, " BYP");
        cv_text(230 - text_w(&FONT_S, t), 4, &FONT_S, t, eng.rec ? N_MAG : N_TXT);
        /* the pad's corners */
        cv_rect(PX0 - 6, PY0 - 6, 8, 1, N_GRID);
        cv_rect(PX0 - 6, PY0 - 6, 1, 8, N_GRID);
        cv_rect(PX1 - 1, PY0 - 6, 8, 1, N_GRID);
        cv_rect(PX1 + 6, PY0 - 6, 1, 8, N_GRID);
        cv_rect(PX0 - 6, PY1 + 6, 8, 1, N_GRID);
        cv_rect(PX0 - 6, PY1 - 1, 1, 8, N_GRID);
        cv_rect(PX1 - 1, PY1 + 6, 8, 1, N_GRID);
        cv_rect(PX1 + 6, PY1 - 1, 1, 8, N_GRID);
        /* the 16 points: a touched one lights, with a ring */
        for (i = 0; i < NWHITE; i++) {
            c = pad_x(patch.key[WK[i]].x * 10);
            r = pad_y(patch.key[WK[i]].y * 10);
            if ((ui.keys >> WK[i] & 1u) || eng.arp_pt == i) {
                g_disc(c, r, 3, N_CYAN);
                g_ring(c, r, 9, mix565(C_BLACK, N_CYAN, 120));
            } else {
                cv_rect(c - 1, r - 1, 3, 3, N_GRID);
            }
        }
        /* gestures playing: their path, dotted, around the cursor */
        for (i = 0; i < NBLACK; i++)
            if (eng.gest_on >> i & 1u) {
                keyp_t *kp = &patch.key[BK[i]];
                int32_t rad = (kp->size ? kp->size : patch.gsize) * (PX1 - PX0) / 200, a;
                for (a = 0; a < 48; a += 2) {
                    float u = (float)a / 48.0f, ox = 0.0f, oy = 0.0f;
                    switch (kp->gest) {
                    case G_CIRCLE: ox = fm_cosf(u * FM_TWO_PI); oy = fm_sinf(u * FM_TWO_PI); break;
                    case G_EIGHT: ox = fm_sinf(u * FM_TWO_PI); oy = 0.5f * fm_sinf(2.0f * u * FM_TWO_PI); break;
                    case G_SWEEPH: ox = 3.2f * u - 1.6f; break;
                    case G_SWEEPV: oy = 3.2f * u - 1.6f; break;
                    case G_DIAG: ox = oy = 2.4f * u - 1.2f; break;
                    case G_SPIRAL: ox = u * fm_cosf(3.0f * u * FM_TWO_PI); oy = u * fm_sinf(3.0f * u * FM_TWO_PI); break;
                    default: ox = 0.25f * fm_cosf(u * FM_TWO_PI); oy = 0.25f * fm_sinf(u * FM_TWO_PI); break;
                    }
                    cv_rect(cx + (int32_t)(ox * (float)rad), cy - (int32_t)(oy * (float)rad), 2, 2,
                            mix565(C_BLACK, N_CYAN, 150));
                }
            }
        /* the trail: one colour to the other, into the black */
        for (i = 22; i >= 0; i--) {
            uint16_t col = i < 8 ? mix565(N_MAG, N_CYAN, i * 32) : mix565(N_CYAN, C_BLACK, (i - 8) * 17);
            cv_line(pad_x(ui.trail_x[i + 1]), pad_y(ui.trail_y[i + 1]), pad_x(ui.trail_x[i]), pad_y(ui.trail_y[i]),
                    col);
        }
        /* the cursor: a glow that grows with the effect and breathes a little; HOLD or a latch: a ring */
        r = 9 + amt * 5 / 1000 + (int32_t)((now / 60u) % 8u < 4u ? (now / 60u) % 4u : 4u - (now / 60u) % 4u) / 2;
        g_disc(cx, cy, r, mix565(C_BLACK, N_MAG, 60 + amt * 60 / 1000));
        g_disc(cx, cy, 6, N_MAG);
        g_disc(cx, cy, 2, mix565(N_MAG, C_WHITE, 160));
        if (eng.hold >= 0 || eng.latched)
            g_ring(cx, cy, r + 4, N_MAG);
        /* the footer: what X and Y move now */
        cv_text(10, 216, &FONT_XS, "X", N_TXT);
        dest_value(patch.xdest[0], t);
        cv_text(22, 216, &FONT_XS, t, N_CYAN);
        cv_text(124, 216, &FONT_XS, "Y", N_TXT);
        dest_value(patch.ydest[0], t);
        cv_text(136, 216, &FONT_XS, t, N_CYAN);
        if (now < ui.ov_until)
            g_overlay(C_WHITE, N_TXT, C_BLACK);
        cv_blit(0, (uint32_t)y0);
    }
}

static void draw_bars(void)
{
    static const uint16_t FILL[SYN_NOSC] = {A_INK, RGB(176, 176, 176), RGB(122, 122, 122), A_BG};
    char t[24];
    int32_t y0, i, x, h, top;
    for (y0 = 0; y0 < 240; y0 += 120) {
        g_strip(y0, 120, A_BG);
        t[0] = 0;
        text_cat(t, "ALG ");
        int_cat(t, patch.algo + 1, 0);
        text_cat(t, " ");
        text_cat(t, SYN_ALGO_NAME[patch.algo]);
        cv_text(12, 5, &FONT_S, t, A_INK);
        hz_text(eng.clock_hz, t);
        cv_text(228 - text_w(&FONT_S, t), 5, &FONT_S, t, A_GRAY);
        cv_rect(12, 26, 216, 1, A_LINE);
        for (i = 0; i < SYN_NOSC; i++) {
            x = 14 + i * 56;
            h = vol[i] * 128 / 100;
            top = 170 - h;
            if (!vol[i]) {
                g_outline(x, 42, 44, 128, A_LINE, 1);
            } else {
                if (i == 3)
                    g_outline(x, top, 44, h, A_INK, 0);
                else
                    cv_rect(x, top, 44, h, FILL[i]);
                if (h > 14) {            /* its wave inside, breathing with the oscillator's level */
                    float full = (float)vol[i] * fm_sqrtf((float)vol[i]), breath = (float)syn_out.level[i] / full;
                    g_wave(x + 4, top + 4, 36, (h - 8 < 40 ? h - 8 : 40), patch.wave[i],
                           0.5f + 0.45f * fm_clampf(breath, 0.0f, 1.0f), i == 3 ? A_INK : A_BG);
                }
            }
            if (i == patch.sel) {
                g_pill(x - 2, 176, 48, 18, A_INK);
                cv_text(x + 22 - text_w(&FONT_S, SYN_WAVE_SHORT[patch.wave[i]]) / 2, 177, &FONT_S,
                        SYN_WAVE_SHORT[patch.wave[i]], A_BG);
            } else {
                cv_text(x + 22 - text_w(&FONT_S, SYN_WAVE_SHORT[patch.wave[i]]) / 2, 177, &FONT_S,
                        SYN_WAVE_SHORT[patch.wave[i]], A_GRAY);
            }
            int_text(patch.semi[i], t, 1);
            cv_text(x + 22 - text_w(&FONT_S, t) / 2, 198, &FONT_S, t, A_INK);
            if (patch.fine[i]) {
                int_text(patch.fine[i], t, 1);
                text_cat(t, "C");
                cv_text(x + 22 - text_w(&FONT_XS, t) / 2, 218, &FONT_XS, t, A_GRAY);
            }
        }
        if (plat_ms() < ui.ov_until)
            g_overlay(A_INK, A_GRAY, A_BG);
        cv_blit(0, (uint32_t)y0);
    }
}

/* a page of rows (style A) in screen rows top..bottom: the rows, the chosen one a white pill; below, its
 * parameters, the chosen one underlined */
static void draw_rows(int32_t top, int32_t bottom, const char *title, const char *right)
{
    int32_t y0, i, first, vis, sel, list_top = top + (title ? 32 : 4);
    char t[32], v[24];
    build(ui.page);
    vis = (bottom - 46 - list_top) / 20;
    sel = nrows ? clamp(ui.row[ui.page], 0, nrows - 1) : 0;
    first = clamp(sel - vis / 2, 0, nrows > vis ? nrows - vis : 0);
    for (y0 = top; y0 < bottom; y0 += 120) {
        int32_t h = bottom - y0 < 120 ? bottom - y0 : 120;
        g_strip(y0, h, A_BG);
        if (title) {
            cv_text(12, top + 5, &FONT_B, title, A_INK);
            if (right)
                cv_text(228 - text_w(&FONT_S, right), top + 5, &FONT_S, right, A_GRAY);
            cv_rect(12, top + 26, 216, 1, A_LINE);
        }
        if (!nrows) {
            cv_text(20, top + 80, &FONT_M, "PRESS A KEY", A_GRAY);
            cv_text(20, top + 110, &FONT_S, "TO SET IT UP", A_GRAY);
        }
        for (i = 0; i < vis && first + i < nrows; i++) {
            row_t *r = &rows[first + i];
            int32_t y = list_top + i * 20, on = first + i == sel;
            int c = on ? clamp(ui.col[ui.page], 0, r->n - 1) : (r->on == 0 && r->n > 1 ? 1 : 0);
            uint16_t ink = on ? A_BG : A_INK, dim = on ? RGB(70, 70, 70) : A_GRAY;
            if (on)
                g_pill(8, y - 1, 224, 19, A_INK);
            if (r->on >= 0) {
                if (col_get(&r->c[(int)r->on]))
                    g_disc(18, y + 8, 3, ink);
                else
                    g_ring(18, y + 8, 4, dim);
            }
            cv_text(28, y, &FONT_S, r->name, ink);
            col_text(&r->c[c], v);
            if (r->n > 1)
                cv_text(118, y + 1, &FONT_XS, r->c[c].name, dim);
            cv_text(226 - text_w(&FONT_S, v), y, &FONT_S, v, ink);
        }
        if (nrows) {                     /* the chosen row's parameters */
            row_t *r = &rows[sel];
            int32_t x = 12, y = bottom - 40, c = clamp(ui.col[ui.page], 0, r->n - 1);
            cv_rect(12, y - 6, 216, 1, A_LINE);
            for (i = 0; i < r->n; i++) {
                int32_t w;
                col_text(&r->c[i], v);
                w = text_w(&FONT_S, v);
                if (text_w(&FONT_XS, r->c[i].name) > w)
                    w = text_w(&FONT_XS, r->c[i].name);
                cv_text(x, y, &FONT_XS, r->c[i].name, i == c ? A_INK : A_GRAY);
                cv_text(x, y + 14, &FONT_S, v, i == c ? A_INK : A_GRAY);
                if (i == c)
                    cv_rect(x, y + 31, w, 2, A_INK);
                x += w + 12;
            }
        }
        (void)t;
        if (plat_ms() < ui.ov_until)
            g_overlay(A_INK, A_GRAY, A_BG);
        cv_blit(0, (uint32_t)y0);
    }
}

static void draw_seq(void)
{
    pattern_t *pt = &pats[patch.pat & 7];
    int32_t i, l = eng_lane, len = pt->len[l] ? pt->len[l] : 16;
    char t[32];
    g_strip(0, 120, A_BG);
    t[0] = 0;
    text_cat(t, "SEQ P");
    int_cat(t, patch.pat + 1, 0);
    text_cat(t, " ");
    text_cat(t, LANE_NAME[l]);
    cv_text(12, 5, &FONT_B, t, A_INK);
    t[0] = 0;
    int_cat(t, ui.seq_page * 16 + 1, 0);
    text_cat(t, "-");
    int_cat(t, ui.seq_page * 16 + 16, 0);
    text_cat(t, eng.rec ? (eng.playing ? "  REC" : "  STEP") : "");
    cv_text(228 - text_w(&FONT_S, t), 5, &FONT_S, t, eng.rec ? A_INK : A_GRAY);
    cv_rect(12, 26, 216, 1, A_LINE);
    /* 16 steps: the KAOS lane's Y as a column, its X as a notch; the OSC lanes' tone up or down */
    for (i = 0; i < 16; i++) {
        int32_t s = ui.seq_page * 16 + i, x = 12 + i * 13 + (i / 4) * 2, ytop = 34, hgt = 74, py, pxv;
        uint16_t frame = s < len ? A_LINE : RGB(30, 30, 30), fill = s == ui.step_held ? A_INK : RGB(120, 120, 120);
        g_outline(x, ytop, 12, hgt, frame, 0);
        if (l == 0) {
            kstep_t *k = &pt->k[s];
            if (k->f & KS_TOUCH) {
                py = ytop + hgt - 2 - k->y * (hgt - 4) / 100;
                cv_rect(x + 1, py, 10, ytop + hgt - 1 - py, fill);
                pxv = ytop + 2 + (100 - k->x) * (hgt - 6) / 100;
                cv_rect(x + 4, pxv, 4, 4, A_BG);
                if (k->g)
                    cv_rect(x + 2, ytop + 2, 8, 2, A_INK);
            }
        } else {
            ostep_t *o = &pt->o[l - 1][s];
            if (o->f & OS_ON) {
                int32_t mid = ytop + hgt / 2, d = o->semi * (hgt / 2 - 2) / 24;
                if (o->f & OS_MUTE)
                    g_outline(x + 2, mid - 3, 8, 6, A_GRAY, 0);
                else if (d >= 0)
                    cv_rect(x + 1, mid - d - 1, 10, d + 2, fill);
                else
                    cv_rect(x + 1, mid - 1, 10, -d + 2, fill);
            }
        }
        if ((eng.playing || eng.rec) && eng.pos[l] == s)
            cv_rect(x + 5, ytop - 4, 3, hgt + 8, C_WHITE);
    }
    if (l == 0) {                        /* the curve through the touched steps */
        int32_t px = -1, pyv = 0;
        for (i = 0; i < 16; i++) {
            kstep_t *k = &pt->k[ui.seq_page * 16 + i];
            int32_t x = 12 + i * 13 + (i / 4) * 2 + 6, y = 34 + 74 - 2 - k->y * 70 / 100;
            if (!(k->f & KS_TOUCH)) {
                px = -1;
                continue;
            }
            if (px >= 0)
                cv_line(px, pyv, x, y, A_INK);
            px = x;
            pyv = y;
        }
    }
    if (plat_ms() < ui.ov_until)
        g_overlay(A_INK, A_GRAY, A_BG);
    cv_blit(0, 0);
    draw_rows(116, 240, 0, 0);           /* the parameters, below the grid */
}

static void draw_slots(void)
{
    int32_t y0, i, first = clamp(ui.slot_sel - 3, 0, NSLOT - 7);
    char t[32], n[NAME_LEN + 1];
    for (y0 = 0; y0 < 240; y0 += 120) {
        g_strip(y0, 120, A_BG);
        cv_text(12, 5, &FONT_B, "SLOTS", A_INK);
        cv_text(228 - text_w(&FONT_S, SLOT_ACT[ui.slot_act]), 5, &FONT_S, SLOT_ACT[ui.slot_act], A_INK);
        cv_rect(12, 26, 216, 1, A_LINE);
        for (i = 0; i < 7; i++) {
            int32_t s = first + i, y = 32 + i * 22, on = s == ui.slot_sel, full;
            uint16_t ink = on ? A_BG : A_INK;
            full = slot_name(s, n);
            if (on)
                g_pill(8, y - 1, 224, 20, A_INK);
            t[0] = 0;
            int_cat(t, s + 1, 0);
            cv_text(16, y, &FONT_S, t, on ? A_BG : A_GRAY);
            cv_text(46, y, &FONT_S, full ? n : "EMPTY", full ? ink : (on ? RGB(80, 80, 80) : A_LINE));
            if (s == sys.slot)
                cv_text(190, y + 1, &FONT_XS, "IN USE", on ? A_BG : A_GRAY);
        }
        cv_rect(12, 190, 216, 1, A_LINE);
        if (ui.naming) {                 /* the name being set, its letter underlined */
            int32_t x = 12;
            cv_text(12, 194, &FONT_XS, "SELECT LETTER  ALGO CHANGE  SAVE DONE", A_GRAY);
            for (i = 0; i < NAME_LEN; i++) {
                char c[2];
                c[0] = patch.name[i] == ' ' ? '_' : patch.name[i];
                c[1] = 0;
                cv_text(x, 210, &FONT_M, c, i == ui.name_pos ? A_INK : A_GRAY);
                if (i == ui.name_pos)
                    cv_rect(x, 235, 14, 2, A_INK);
                x += 18;
            }
        } else {
            cv_text(12, 198, &FONT_XS, "SELECT SLOT   ALGO ACTION   SAVE DOES IT", A_GRAY);
            t[0] = 0;
            text_cat(t, "NOW: ");
            for (i = 0; i < NAME_LEN; i++)
                n[i] = patch.name[i];
            n[NAME_LEN] = 0;
            text_cat(t, n);
            cv_text(12, 216, &FONT_S, t, A_INK);
        }
        if (plat_ms() < ui.ov_until)
            g_overlay(A_INK, A_GRAY, A_BG);
        cv_blit(0, (uint32_t)y0);
    }
}

void app_splash(void)
{
    int32_t y0, i;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    for (y0 = 0; y0 < 240; y0 += 120) {
        g_strip(y0, 120, C_BLACK);
        for (i = 0; i < 64; i++) {       /* a dot drew a circle and left a trail */
            float a = (float)i / 64.0f * 0.9f;
            int32_t x = 120 + (int32_t)(78.0f * fm_cosf(a * FM_TWO_PI - 1.2f));
            int32_t y = 104 + (int32_t)(78.0f * fm_sinf(a * FM_TWO_PI - 1.2f));
            g_disc(x, y, 2, mix565(N_CYAN, N_MAG, i * 4));
        }
        g_disc(120 + (int32_t)(78.0f * fm_cosf(0.9f * FM_TWO_PI - 1.2f)),
               104 + (int32_t)(78.0f * fm_sinf(0.9f * FM_TWO_PI - 1.2f)), 7, N_MAG);
        cv_text(120 - text_w(&FONT_L, APP_NAME) / 2, 88, &FONT_L, APP_NAME, C_WHITE);
        cv_text(120 - text_w(&FONT_XS, MD_VERSION) / 2, 210, &FONT_XS, MD_VERSION, N_TXT);
        cv_blit(0, (uint32_t)y0);
    }
}

static void speak(void)                  /* a screen reader that has just started listening hears the page */
{
    char t[96];
    if (ann_on() && ann_where()) {
        t[0] = 0;
        text_cat(t, APP_NAME ". ");
        text_cat(t, PAGE_NAME[ui.page]);
        text_cat(t, ". ALGO ");
        text_cat(t, SYN_ALGO_NAME[patch.algo]);
        text_cat(t, ". ");
        text_cat(t, ENV_NAME[patch.env & 7]);
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

/* -------------------------------------------------------------- contract --- */
void app_init(void)
{
    int i;
    syn_init();
    sys_load();
    if (slot_load(sys.slot))
        patch_defaults();
    for (i = 0; i < SYN_NOSC; i++)
        vol[i] = 0;                      /* silent at every power-on */
    eng_init();
    pad_keys = 0;
    eng_lane = 0;
    fx_bypass = 0;
    patch_dirty = 0;
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    eng_block(n);
    syn_render(lr, n, master_q12);
}

void app_panic(void) { syn_panic(); }

void app_ui_init(void)
{
    int i;
    for (i = 0; i < NPAGE; i++)
        ui.row[i] = ui.col[i] = 0;
    ui.page = ui.main = P_KAOS;
    ui.glo_key = -1;
    ui.glo_copy = -1;
    ui.step_held = -1;
    ui.slot_sel = sys.slot;
    ui.shift_lock = 0;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.redraw = 1;
}

void app_frame(void)
{
    uint32_t now = plat_ms(), i, moving;
    static int32_t last_cx = -1, last_cy = -1, last_amt = -1;
    static uint32_t last_pos, last_pulse;
    input();
    speak();
    leds();
    moving = eng.cx != last_cx || eng.cy != last_cy || eng.amt != last_amt;
    if (now - ui.trail_t >= 30u) {       /* the trail: a place every 30 ms, so its length is a time */
        ui.trail_t = now;
        for (i = 23; i > 0; i--) {
            ui.trail_x[i] = ui.trail_x[i - 1];
            ui.trail_y[i] = ui.trail_y[i - 1];
        }
        ui.trail_x[0] = eng.cx;
        ui.trail_y[0] = eng.cy;
        if (ui.trail_x[23] != ui.trail_x[0] || ui.trail_y[23] != ui.trail_y[0])
            moving = 1;
    }
    if (ui.ov_until && now >= ui.ov_until) {
        ui.ov_until = 0;
        ui.redraw = 1;
    }
    if (ui.page == P_BARS && (vol[0] | vol[1] | vol[2] | vol[3]))
        moving = 1;                      /* the waves breathe */
    if (ui.page == P_KAOS && eng.pulses != last_pulse)
        moving = 1;                      /* the clock's dot */
    if (ui.page == P_SEQ && (uint32_t)eng.pos[eng_lane] != last_pos)
        ui.redraw = 1;
    if (ui.page == P_SYS && now - ui.drawn_at >= 500u)
        ui.redraw = 1;                   /* the audio's load, live */
    if (ui.page != P_KAOS && ui.page != P_BARS)
        moving = 0;
    if (moving && now - ui.drawn_at >= 33u)
        ui.redraw = 1;
    if (ui.redraw && now - ui.drawn_at >= 16u) {
        ui.redraw = 0;
        ui.drawn_at = now;
        last_cx = eng.cx;
        last_cy = eng.cy;
        last_amt = eng.amt;
        last_pos = (uint32_t)eng.pos[eng_lane];
        last_pulse = eng.pulses;
        switch (ui.page) {
        case P_KAOS: draw_kaos(); break;
        case P_BARS: draw_bars(); break;
        case P_SEQ: draw_seq(); break;
        case P_SAVE: draw_slots(); break;
        default: {
            char right[16] = "";
            build(ui.page);
            if (ui.page == P_GLO && ui.glo_key >= 0) {
                text_cat(right, "KEY ");
                key_name(ui.glo_key, right + 4);
            } else if (nrows) {
                int_cat(right, clamp(ui.row[ui.page], 0, nrows - 1) + 1, 0);
                text_cat(right, "/");
                int_cat(right, nrows, 0);
            }
            draw_rows(0, 240, PAGE_NAME[ui.page], right);
            break;
        }
        }
    }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    static const struct {
        const char *name;
        int kind;
    } NAMES[] = {{"page", 0}, {"sel", 1}, {"algo", 2}, {"wave", 3}, {"semi", 4}, {"fine", 5}, {"cx", 6}, {"cy", 7},
                 {"amt", 8}, {"shift", 9}, {"dirty", 10}, {"playing", 11}, {"rec", 12}, {"pos", 13}, {"hold", 14},
                 {"gest", 15}, {"latched", 16}, {"arp_pt", 17}, {"clock", 18}, {"env", 19}, {"slot", 20},
                 {"pat", 21}, {"lane", 22}, {"row", 23}, {"col", 24}, {"glo_key", 25}, {"bypass", 26},
                 {"fxlevel", 27}, {"cutoff", 28}, {"pulses", 29}, {"drift", 30}, {"midi_ch", 31}};
    int i, k = -1;
    if (!strncmp(what, "vol", 3)) {      /* volN: oscillator N's (1..4) */
        *got = vol[(atoi(what + 3) + 3) % SYN_NOSC];
        return 1;
    }
    if (!strncmp(what, "kstep", 5)) {    /* kstepN: step N touched on the KAOS lane */
        *got = pats[patch.pat & 7].k[atoi(what + 5) % NSTEP].f & KS_TOUCH;
        return 1;
    }
    if (!strncmp(what, "ostep", 5)) {    /* ostepN: step N on, OSC1's lane */
        *got = pats[patch.pat & 7].o[0][atoi(what + 5) % NSTEP].f & OS_ON;
        return 1;
    }
    if (!strncmp(what, "fxon", 4)) {
        *got = patch.fx[atoi(what + 4) % NFX].on;
        return 1;
    }
    if (!strncmp(what, "fxamt", 5)) {
        *got = patch.fx[atoi(what + 5) % NFX].amt;
        return 1;
    }
    for (i = 0; i < (int)(sizeof NAMES / sizeof NAMES[0]); i++)
        if (!strcmp(what, NAMES[i].name))
            k = NAMES[i].kind;
    switch (k) {
    case 0: *got = ui.page; break;
    case 1: *got = patch.sel; break;
    case 2: *got = patch.algo; break;
    case 3: *got = patch.wave[patch.sel]; break;
    case 4: *got = patch.semi[patch.sel]; break;
    case 5: *got = patch.fine[patch.sel]; break;
    case 6: *got = eng.cx; break;
    case 7: *got = eng.cy; break;
    case 8: *got = eng.amt; break;
    case 9: *got = shift(); break;
    case 10: *got = patch_dirty; break;
    case 11: *got = eng.playing; break;
    case 12: *got = eng.rec; break;
    case 13: *got = eng.pos[eng_lane]; break;
    case 14: *got = eng.hold; break;
    case 15: *got = (int)eng.gest_on; break;
    case 16: *got = (int)eng.latched; break;
    case 17: *got = eng.arp_pt; break;
    case 18: *got = patch.clock_chz; break;
    case 19: *got = patch.env; break;
    case 20: *got = sys.slot; break;
    case 21: *got = patch.pat; break;
    case 22: *got = eng_lane; break;
    case 23: *got = ui.row[ui.page]; break;
    case 24: *got = ui.col[ui.page]; break;
    case 25: *got = ui.glo_key; break;
    case 26: *got = fx_bypass; break;
    case 27: *got = patch.fx_level; break;
    case 28: *got = eng.cutoff_hz; break;
    case 29: *got = (int)eng.pulses; break;
    case 30: *got = patch.drift; break;
    case 31: *got = sys.midi_ch; break;
    default: return 0;
    }
    return 1;
}
#endif
