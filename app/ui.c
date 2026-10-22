/* MODELAR: the panel and the screen. Part of the unity build (device/unity.c, host/plat_host.c), after the
 * platform: gfx.c and announce.c are in scope.
 *
 *   white keys   the KAOS PAD: 16 points on a 4 x 4 grid (the group of four is the row, Y; the place in it the
 *                column, X); several held, the cursor goes between them
 *   SELECT       the active oscillator          PRESET  its wave          ALGO  the routing
 *   K1..K4       the oscillators' volumes (all start at 0: the sound comes in as they rise); SEL + K: fine tune
 *   OCT- / OCT+  the active oscillator's tone, a semitone (SEL: an octave); both: back to 0 (SEL: all four)
 *   HOME         KAOS PAD <-> 4 BARS            SAVE    the patch          SEL  SHIFT (held, or two taps: locked)
 */
#include "fastmath.h"

#define PATCH_MAGIC 0x4C444F4Du          /* "MODL" */
#define PATCH_FORMAT 1u
typedef struct {
    uint32_t magic, format;
    int8_t wave[SYN_NOSC], semi[SYN_NOSC], fine[SYN_NOSC];
    uint8_t algo, page, sel, rsv0;
    int16_t slew_ms, attack_ms, release_ms, range;
    uint8_t rsv[16];                     /* room to grow */
} patch_t;
static patch_t patch;
static int32_t vol[SYN_NOSC];            /* never saved: every power-on and every patch starts silent */
static uint8_t patch_dirty;

enum { PG_KAOS, PG_BARS };

static void patch_defaults(void)
{
    static const int8_t W[SYN_NOSC] = {W_SAW, W_SINE, W_SQUARE, W_NOISE}, S[SYN_NOSC] = {0, 7, -12, 3};
    int i;
    patch.magic = PATCH_MAGIC;
    patch.format = PATCH_FORMAT;
    for (i = 0; i < SYN_NOSC; i++) {
        patch.wave[i] = W[i];
        patch.semi[i] = S[i];
        patch.fine[i] = 0;
    }
    patch.algo = A_CROSS;
    patch.page = PG_KAOS;
    patch.sel = 0;
    patch.slew_ms = 300;
    patch.attack_ms = 80;
    patch.release_ms = 600;
    patch.range = 85;
}

static int patch_ok(void)
{
    int i;
    if (patch.magic != PATCH_MAGIC || patch.format != PATCH_FORMAT || patch.algo >= SYN_NALGOS || patch.page > 1u
        || patch.sel >= SYN_NOSC)
        return 0;
    for (i = 0; i < SYN_NOSC; i++)
        if (patch.wave[i] < 0 || patch.wave[i] >= SYN_NWAVES || patch.semi[i] < -48 || patch.semi[i] > 48
            || patch.fine[i] < -50 || patch.fine[i] > 50)
            return 0;
    return 1;
}

/* the patch and the volumes into the instrument */
static void patch_apply(void)
{
    int i;
    for (i = 0; i < SYN_NOSC; i++) {
        syn_in.wave[i] = patch.wave[i];
        syn_in.semi[i] = patch.semi[i];
        syn_in.fine[i] = patch.fine[i];
        syn_in.vol[i] = vol[i];
    }
    syn_in.algo = patch.algo;
    syn_in.slew_ms = patch.slew_ms;
    syn_in.attack_ms = patch.attack_ms;
    syn_in.release_ms = patch.release_ms;
    syn_in.range = patch.range;
}

/* ------------------------------------------------------------------- state --- */
static struct {
    uint32_t keys, btn;                  /* held at the last input */
    uint8_t shift_lock;                  /* SEL tapped twice: SHIFT stays on (its LED blinks); a tap lets go */
    uint32_t sel_down, sel_tap;          /* when SEL last went down; when a clean tap of it ended */
    uint8_t sel_used;                    /* something else was played while SEL was held: no tap */
    uint32_t oct_t, oct_next;            /* OCT- / OCT+ held: when, and the next repeat */
    char ov_name[16], ov_val[24];        /* the value just changed, over the screen for a moment */
    int32_t ov_bar;                      /* its bar, 0..1000 (-1 none) */
    int32_t ov_wave;                     /* its wave's picture (-1 none) */
    uint32_t ov_until;
    int32_t trail_x[24], trail_y[24];    /* the cursor's last places (0..1000), newest first */
    uint32_t trail_t;
    uint8_t redraw;
    uint32_t drawn_at;
} ui;

static const uint8_t WK[16] = PLAT_WHITE_KEYS;

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

static void say_vol(int i)
{
    char n[8], v[8];
    osc_name(i, n);
    int_text(vol[i], v, 0);
    app_say(n, v);
    ui.ov_bar = vol[i] * 10;
}

static void say_tone(int i)
{
    char n[8], v[16], c[8];
    osc_name(i, n);
    int_text(patch.semi[i], v, 1);
    if (patch.fine[i]) {
        text_cat(v, " ");
        int_text(patch.fine[i], c, 1);
        text_cat(v, c);
        text_cat(v, "C");
    }
    app_say(n, v);
}

/* ------------------------------------------------------------------- input --- */
static void tone_step(int32_t d)
{
    int i = patch.sel;
    int32_t s = clamp(patch.semi[i] + d, -48, 48);
    if (s != patch.semi[i]) {
        patch.semi[i] = (int8_t)s;
        patch_dirty = 1;
    }
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

static void save(void)
{
    if (plat_store_save(OBJ_PATCH, &patch, sizeof patch) == 0) {
        patch_dirty = 0;
        app_say("SAVED", "");
    } else {
        app_say("SAVE", "FAILED");
    }
}

static void button(uint32_t b)
{
    static const char *const LATER[NB] = {"v0.2", 0, "v0.3", "v0.3", "v0.2", "v0.3", 0, 0, "v0.2", "v0.2", "v0.2",
                                          "v0.2", 0, 0};
    static const char *const NAME[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ",
                                         "PLAY", "REC", "OCT-", "OCT+"};
    uint32_t both = 1u << B_OCTDN | 1u << B_OCTUP;
    ui.sel_used = 1;
    switch (b) {
    case B_SEL:                          /* SHIFT: held; two clean taps lock it, a tap lets it go */
        if (ui.shift_lock)
            ui.shift_lock = 0;
        else if (ui.sel_tap && plat_ms() - ui.sel_tap < 300u)
            ui.shift_lock = 1;
        ui.sel_down = plat_ms();
        ui.sel_used = 0;
        ui.sel_tap = 0;
        return;
    case B_HOME:
        if (shift()) {
            app_say("SYSTEM", "v0.3");
        } else {
            patch.page ^= 1u;
            lcd_fill(0, 0, 240, 240, patch.page == PG_KAOS ? C_BLACK : RGB(16, 16, 16));
        }
        break;
    case B_SAVE:
        save();
        break;
    case B_OCTDN:
    case B_OCTUP:
        if ((ui.btn & both) == both || (plat_buttons() & both) == both) {
            tone_reset();
            ui.oct_t = 0;
        } else {
            tone_step((b == B_OCTUP ? 1 : -1) * (shift() ? 12 : 1));
            ui.oct_t = plat_ms();
            ui.oct_next = ui.oct_t + 400u;
        }
        break;
    default:
        app_say(NAME[b], LATER[b] ? LATER[b] : "");
        break;
    }
    ui.redraw = 1;
}

static void encoder(uint32_t e, int32_t d)
{
    char t[24];
    int i = patch.sel;
    ui.sel_used = 1;
    switch (e) {
    case EN_SELECT:
        if (shift()) {
            app_say("CLOCK", "v0.2");
            break;
        }
        patch.sel = (uint8_t)clamp(patch.sel + d, 0, SYN_NOSC - 1);
        osc_name(patch.sel, t);
        text_cat(t, " ");
        text_cat(t, SYN_WAVE_SHORT[patch.wave[patch.sel]]);
        app_say("ACTIVE", t);
        break;
    case EN_PRESET:
        if (shift()) {
            app_say("PATCH", "v0.3");
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
            app_say("FX DRY/WET", "v0.2");
            break;
        }
        patch.algo = (uint8_t)clamp(patch.algo + d, 0, SYN_NALGOS - 1);
        patch_dirty = 1;
        int_text(patch.algo + 1, t, 0);
        text_cat(t, " ");
        text_cat(t, SYN_ALGO_NAME[patch.algo]);
        app_say("ALGO", t);
        break;
    default:                             /* K1..K4 */
        i = (int)(e - EN_K1);
        if (shift()) {
            int32_t f = clamp(patch.fine[i] + d, -50, 50);
            if (f != patch.fine[i]) {
                patch.fine[i] = (int8_t)f;
                patch_dirty = 1;
            }
            say_tone(i);
        } else {
            vol[i] = clamp(vol[i] + d * 2, 0, 100);
            say_vol(i);
        }
        break;
    }
}

static void input(void)
{
    uint32_t keys = plat_keys(), btn = plat_buttons(), k, now = plat_ms(), n = 0;
    int32_t sx = 0, sy = 0, e;
    for (k = 0; k < NB; k++)
        if ((btn >> k & 1u) && !(ui.btn >> k & 1u)) {
            ui.btn = btn & ~(1u << k);   /* (button() sees the others as held) */
            button(k);
        }
    if ((ui.btn >> B_SEL & 1u) && !(btn >> B_SEL & 1u))   /* SEL let go: a clean tap, or SHIFT used */
        ui.sel_tap = !ui.sel_used && now - ui.sel_down < 250u && !ui.shift_lock ? now : 0;
    ui.btn = btn;
    if (ui.oct_t && (btn & (1u << B_OCTDN | 1u << B_OCTUP)) && now >= ui.oct_next) {   /* held: on and on */
        tone_step((btn >> B_OCTUP & 1u ? 1 : -1) * (shift() ? 12 : 1));
        ui.oct_next = now + 100u;
    } else if (!(btn & (1u << B_OCTDN | 1u << B_OCTUP))) {
        ui.oct_t = 0;
    }
    for (k = 0; k < NE; k++)
        if ((e = plat_enc((int)k)) != 0)
            encoder(k, e);
    /* the pad: the white keys held, their points averaged */
    for (k = 0; k < 16u; k++)
        if (keys >> WK[k] & 1u) {
            sx += (int32_t)(k % 4u) * 1000 / 3;
            sy += (int32_t)(k / 4u) * 1000 / 3;
            n++;
        }
    if (n) {
        syn_in.tx = sx / (int32_t)n;
        syn_in.ty = sy / (int32_t)n;
    }
    syn_in.touch = (int32_t)n;
    if (keys != ui.keys) {
        ui.redraw = 1;
        ui.sel_used = 1;
    }
    ui.keys = keys;
    patch_apply();
}

static uint32_t nearest_key(void)        /* the white key whose point is nearest the cursor */
{
    int32_t c = (syn_out.cx * 3 + 500) / 1000, r = (syn_out.cy * 3 + 500) / 1000;
    return WK[clamp(r, 0, 3) * 4 + clamp(c, 0, 3)];
}

static void leds(void)
{
    uint32_t b = 0, now = plat_ms();
    if (ui.shift_lock ? (now / 250u) & 1u : ui.btn >> B_SEL & 1u)
        b |= 1u << B_SEL;
    if (patch_dirty)
        b |= 1u << B_SAVE;
    if (patch.page == PG_BARS)
        b |= 1u << B_HOME;
    plat_glow(1);
    plat_leds(b, ui.keys | 1u << nearest_key());
}

void app_input_only(void) { input(); }

/* ------------------------------------------------------------------ screen --- */
/* the KAOS PAD's neon (style B) and the pages' soft black and white (style A) */
#define N_MAG RGB(255, 46, 136)
#define N_CYAN RGB(0, 229, 255)
#define N_GRID RGB(43, 36, 64)
#define N_TXT RGB(153, 153, 153)
#define A_BG RGB(16, 16, 16)
#define A_INK RGB(232, 232, 232)
#define A_GRAY RGB(138, 138, 138)
#define A_LINE RGB(58, 58, 58)

static uint16_t mix565(uint16_t a, uint16_t b, int32_t t)   /* a .. b, t 0..256 */
{
    int32_t r = (a >> 11) + (((b >> 11) - (a >> 11)) * t >> 8);
    int32_t g = ((a >> 5) & 63) + ((((b >> 5) & 63) - ((a >> 5) & 63)) * t >> 8);
    int32_t bl = (a & 31) + (((b & 31) - (a & 31)) * t >> 8);
    return (uint16_t)(r << 11 | g << 5 | bl);
}

/* a strip of the screen, rows y0 .. y0+h-1, drawn in screen coordinates */
static void g_strip(int32_t y0, int32_t h, uint16_t bg)
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

static void g_wave_pic(int32_t x, int32_t y, int32_t w, int32_t h, int32_t wave, float amp, uint16_t c)
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

static void hz_text(int32_t hz, char *t)
{
    if (hz >= 1000) {
        char d[4] = {'.', 0, 'K', 0};
        int_text(hz / 1000, t, 0);
        d[1] = (char)('0' + hz / 100 % 10);
        text_cat(t, d);
    } else {
        int_text(hz, t, 0);
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
        g_wave_pic(x + 12 > 160 ? 160 : x + 12, 20, 66, 30, ui.ov_wave, 0.9f, ink);
    if (ui.ov_bar >= 0) {
        cv_rect(12, 54, 216, 2, A_LINE);
        cv_rect(12, 54, 216 * ui.ov_bar / 1000, 2, ink);
    }
}

static void draw_kaos(void)
{
    char t[24];
    int32_t y0, i, c, r, cx = pad_x(syn_out.cx), cy = pad_y(syn_out.cy), amt = syn_out.amt;
    uint32_t now = plat_ms();
    for (y0 = 0; y0 < 240; y0 += 120) {
        g_strip(y0, 120, C_BLACK);
        /* the header: a dot that breathes with the pad's effect, the routing, the active oscillator */
        g_disc(12, 13, 3 + amt * 2 / 1000, mix565(N_GRID, N_MAG, 80 + amt * 176 / 1000));
        t[0] = 0;
        int_text(patch.algo + 1, t, 0);
        text_cat(t, " ");
        text_cat(t, SYN_ALGO_NAME[patch.algo]);
        cv_text(24, 5, &FONT_S, t, N_TXT);
        osc_name(patch.sel, t);
        text_cat(t, " ");
        text_cat(t, SYN_WAVE_SHORT[patch.wave[patch.sel]]);
        cv_text(228 - text_w(&FONT_S, t), 5, &FONT_S, t, N_CYAN);
        /* the pad's corners */
        cv_rect(PX0 - 6, PY0 - 6, 8, 1, N_GRID);
        cv_rect(PX0 - 6, PY0 - 6, 1, 8, N_GRID);
        cv_rect(PX1 - 1, PY0 - 6, 8, 1, N_GRID);
        cv_rect(PX1 + 6, PY0 - 6, 1, 8, N_GRID);
        cv_rect(PX0 - 6, PY1 + 6, 8, 1, N_GRID);
        cv_rect(PX0 - 6, PY1 - 1, 1, 8, N_GRID);
        cv_rect(PX1 - 1, PY1 + 6, 8, 1, N_GRID);
        cv_rect(PX1 + 6, PY1 - 1, 1, 8, N_GRID);
        /* the 16 points: a touched one lights cyan, with a ring */
        for (i = 0; i < 16; i++) {
            c = pad_x((i % 4) * 1000 / 3);
            r = pad_y((i / 4) * 1000 / 3);
            if (ui.keys >> WK[i] & 1u) {
                g_disc(c, r, 3, N_CYAN);
                g_ring(c, r, 9, mix565(C_BLACK, N_CYAN, 120));
            } else {
                cv_rect(c - 1, r - 1, 3, 3, N_GRID);
            }
        }
        /* the trail: magenta to cyan, into the black */
        for (i = 22; i >= 0; i--) {
            uint16_t col = i < 8 ? mix565(N_MAG, N_CYAN, i * 32) : mix565(N_CYAN, C_BLACK, (i - 8) * 17);
            cv_line(pad_x(ui.trail_x[i + 1]), pad_y(ui.trail_y[i + 1]), pad_x(ui.trail_x[i]), pad_y(ui.trail_y[i]),
                    col);
        }
        /* the cursor: a glow that grows with the effect and breathes a little */
        r = 9 + amt * 5 / 1000 + (int32_t)((now / 60u) % 8u < 4u ? (now / 60u) % 4u : 4u - (now / 60u) % 4u) / 2;
        g_disc(cx, cy, r, mix565(C_BLACK, N_MAG, 60 + amt * 60 / 1000));
        g_disc(cx, cy, 6, N_MAG);
        g_disc(cx, cy, 2, mix565(N_MAG, C_WHITE, 160));
        /* the footer: what X and Y move, and how far */
        cv_text(12, 216, &FONT_S, "X CUT", N_TXT);
        t[0] = 0;
        hz_text(syn_out.cutoff_hz, t);
        cv_text(52, 216, &FONT_S, t, N_CYAN);
        cv_text(128, 216, &FONT_S, "Y XMOD", N_TXT);
        t[0] = 0;
        int_text(syn_out.xmod / 10, t, 0);
        text_cat(t, "%");
        cv_text(176, 216, &FONT_S, t, N_CYAN);
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
        int_text(patch.algo + 1, t + 4, 0);
        text_cat(t, " ");
        text_cat(t, SYN_ALGO_NAME[patch.algo]);
        cv_text(12, 5, &FONT_S, t, A_INK);
        t[0] = 0;
        int_text((int32_t)(plat_master() * 100u / 4096u), t, 0);
        text_cat(t, "%");
        cv_text(228 - text_w(&FONT_S, t), 5, &FONT_S, t, A_GRAY);
        cv_text(228 - text_w(&FONT_S, t) - 6 - text_w(&FONT_XS, "VOL"), 6, &FONT_XS, "VOL", A_LINE);
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
                    float full = (float)vol[i] * fm_sqrtf((float)vol[i]) * 10.0f * 0.1f, breath = (float)syn_out.level[i] / full;
                    g_wave_pic(x + 4, top + 4, 36, (h - 8 < 40 ? h - 8 : 40), patch.wave[i],
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

void app_splash(void)
{
    int32_t y0, i;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    for (y0 = 0; y0 < 240; y0 += 120) {
        g_strip(y0, 120, C_BLACK);
        for (i = 0; i < 64; i++) {       /* a dot drew a circle and left a trail: magenta to cyan */
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

/* a screen reader that has just started listening (or asks) hears the whole screen */
static void speak(void)
{
    char t[96], n[8];
    int i;
    if (ann_on() && ann_where()) {
        t[0] = 0;
        text_cat(t, APP_NAME ". ");
        text_cat(t, patch.page == PG_KAOS ? "KAOS PAD. " : "BARS. ");
        text_cat(t, "ALGO ");
        text_cat(t, SYN_ALGO_NAME[patch.algo]);
        for (i = 0; i < SYN_NOSC; i++) {
            osc_name(i, n);
            text_cat(t, ". ");
            text_cat(t, n);
            text_cat(t, " ");
            text_cat(t, SYN_WAVE_SHORT[patch.wave[i]]);
        }
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

static void midi_drain(void)             /* v0.1 listens to no MIDI yet: what comes in is let go */
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port))
        ;
}

/* -------------------------------------------------------------- contract --- */
void app_init(void)
{
    int i;
    for (i = 0; i < SYN_NOSC; i++)
        vol[i] = 0;                      /* silent at every power-on */
    syn_init();
    if (plat_store_load(OBJ_PATCH, &patch, sizeof patch) != (int)sizeof patch || !patch_ok())
        patch_defaults();
    patch_apply();
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12) { syn_render(lr, n, master_q12); }

void app_panic(void) { syn_panic(); }

void app_ui_init(void)
{
    lcd_fill(0, 0, 240, 240, patch.page == PG_KAOS ? C_BLACK : A_BG);
    ui.redraw = 1;
}

void app_frame(void)
{
    uint32_t now = plat_ms(), i, moving;
    static int32_t last_cx = -1, last_cy = -1, last_amt = -1;
    input();
    midi_drain();
    speak();
    leds();
    moving = syn_out.cx != last_cx || syn_out.cy != last_cy || syn_out.amt != last_amt;
    if (now - ui.trail_t >= 30u) {       /* the trail: a place every 30 ms, so its length is a time */
        ui.trail_t = now;
        for (i = 23; i > 0; i--) {
            ui.trail_x[i] = ui.trail_x[i - 1];
            ui.trail_y[i] = ui.trail_y[i - 1];
        }
        ui.trail_x[0] = syn_out.cx;
        ui.trail_y[0] = syn_out.cy;
        if (ui.trail_x[23] != ui.trail_x[0] || ui.trail_y[23] != ui.trail_y[0])
            moving = 1;
    }
    if (ui.ov_until && now >= ui.ov_until) {
        ui.ov_until = 0;
        ui.redraw = 1;
    }
    if (patch.page == PG_BARS && (vol[0] | vol[1] | vol[2] | vol[3]))
        moving = 1;                      /* the waves breathe */
    if (moving && now - ui.drawn_at >= 33u)
        ui.redraw = 1;
    if (ui.redraw) {
        ui.redraw = 0;
        ui.drawn_at = now;
        last_cx = syn_out.cx;
        last_cy = syn_out.cy;
        last_amt = syn_out.amt;
        if (patch.page == PG_KAOS)
            draw_kaos();
        else
            draw_bars();
    }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "page"))
        *got = patch.page;
    else if (!strcmp(what, "sel"))
        *got = patch.sel;
    else if (!strcmp(what, "algo"))
        *got = patch.algo;
    else if (!strcmp(what, "wave"))      /* the active oscillator's */
        *got = patch.wave[patch.sel];
    else if (!strcmp(what, "semi"))
        *got = patch.semi[patch.sel];
    else if (!strcmp(what, "fine"))
        *got = patch.fine[patch.sel];
    else if (!strncmp(what, "vol", 3))   /* volN: oscillator N's (1..4) */
        *got = vol[(atoi(what + 3) + 3) % SYN_NOSC];
    else if (!strcmp(what, "touch"))
        *got = syn_in.touch;
    else if (!strcmp(what, "tx"))
        *got = syn_in.tx;
    else if (!strcmp(what, "ty"))
        *got = syn_in.ty;
    else if (!strcmp(what, "cx"))
        *got = syn_out.cx;
    else if (!strcmp(what, "cy"))
        *got = syn_out.cy;
    else if (!strcmp(what, "amt"))
        *got = syn_out.amt;
    else if (!strcmp(what, "shift"))
        *got = shift();
    else if (!strcmp(what, "dirty"))
        *got = patch_dirty;
    else
        return 0;
    return 1;
}
#endif
