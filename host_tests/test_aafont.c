/* For mkdir, which C11 alone does not declare. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "aafont.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * The anti-aliased typeface on the host: that the generated tables are what
 * aafont.c assumes (sorted, in bounds, every face with its degree sign and
 * tabular figures), that the blend is linear-light arithmetic to the code,
 * that text never writes outside the framebuffer wherever it is put, that
 * aafont_width is exactly the ink aafont_draw lays down and each alignment
 * puts that ink where it says -- and a specimen sheet of every face and
 * every glyph, with envo's pages mocked up at their real size, to look at.
 *
 * The specimen goes to host_tests/renders/aafont/ as 24-bit BMPs, at 1x and
 * at three times the size (pixels repeated, not smoothed). Turn each into a
 * PNG with
 *     sips -s format png X.bmp --out X.png
 */

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

typedef struct {
    const aafont_t *f;
    const char *name;
    const char *face;        /* for the specimen's captions */
    const char *must;        /* characters it must have */
    bool tabular;            /* all ten figures one width */
} face_t;

#define ASCII " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"
#define WORDSET " .,:+-%/0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZes"

/* inter_sub's figures are proportional: it sets dates, not columns. */
static const face_t s_faces[] = {
    { &aafont_inter_label,  "inter_label",  "Inter Medium 17 px",   ASCII, false }, /* proportional: "eCO2" must not open up */
    { &aafont_inter_sub,    "inter_sub",    "Inter Medium 23 px",   ASCII, false },
    { &aafont_inter_word,   "inter_word",   "Inter SemiBold 34 px", WORDSET, true },
    { &aafont_inter_state,  "inter_state",  "Inter SemiBold 44 px", WORDSET, true },
    { &aafont_inter_number, "inter_number", "Inter SemiBold 64 px", " .:+-%0123456789", true },
    { &aafont_inter_reading, "inter_reading", "Inter SemiBold 77 px", " .-0123456789", true },
};
#define FACES ((int)(sizeof s_faces / sizeof s_faces[0]))

/* POOR a weight up, for the knock-out, beside the face each stands in for. */
typedef struct {
    const aafont_t *f, *base;
    const char *name, *face;
} knock_t;

static const knock_t s_knocks[] = {
    { &aafont_inter_label_knock, &aafont_inter_label, "inter_label_knock", "Inter SemiBold 17 px" },
    { &aafont_inter_sub_knock,   &aafont_inter_sub,   "inter_sub_knock",   "Inter SemiBold 23 px" },
    { &aafont_inter_word_knock,  &aafont_inter_word,  "inter_word_knock",  "Inter Bold 34 px" },
    { &aafont_inter_state_knock, &aafont_inter_state, "inter_state_knock", "Inter Bold 44 px" },
};
#define KNOCKS ((int)(sizeof s_knocks / sizeof s_knocks[0]))

#define DEG "\xC2\xB0"
#define MINUS "\xE2\x88\x92"

/* ---- colours, and RGB565 to light ------------------------------------------ */

#define BLACK 0x0000
#define WHITE 0xFFFF
#define GREY  0x8410
#define GOOD  0x04BF
#define FAIR  0xFD40
#define POOR  0xF8C1
#define RULE  0x5ACB

/* A code point of the tables' (16 bits) as a UTF-8 string, in out[4]. */
static void utf8_of(uint16_t cp, char out[4])
{
    if (cp < 0x80) { out[0] = (char)cp; out[1] = 0; }
    else if (cp < 0x800) { out[0] = (char)(0xC0 | cp >> 6); out[1] = (char)(0x80 | (cp & 63)); out[2] = 0; }
    else {
        out[0] = (char)(0xE0 | cp >> 12);
        out[1] = (char)(0x80 | ((cp >> 6) & 63));
        out[2] = (char)(0x80 | (cp & 63));
        out[3] = 0;
    }
}

static const aafont_glyph_t *glyph_of(const aafont_t *f, uint32_t cp)
{
    for (int i = 0; i < f->count; i++)
        if (f->glyphs[i].cp == cp) return &f->glyphs[i];
    return NULL;
}

/* ---- the tables ---------------------------------------------------------- */

static void test_tables(void)
{
    char what[160];
    for (int k = 0; k < FACES; k++) {
        const aafont_t *f = s_faces[k].f;
        bool sorted = true, inside = true, kerns_ok = true, has_all = true, tabular = true;
        for (int i = 0; i < f->count; i++) {
            const aafont_glyph_t *g = &f->glyphs[i];
            if (i > 0 && g->cp <= f->glyphs[i - 1].cp) sorted = false;
            if (g->off + (uint32_t)((g->w + 1) / 2) * g->h > f->bits_len) inside = false;
            /* No bitmap may have an all-blank edge: the crop is to the ink,
               and aafont_width counts on it. */
            if (g->w > 0 && g->h > 0) {
                int stride = (g->w + 1) / 2;
                const uint8_t *b = f->bits + g->off;
                bool top = false, bottom = false, left = false, right = false;
                for (int x = 0; x < g->w; x++) {
                    int a0 = x & 1 ? b[x / 2] & 15 : b[x / 2] >> 4;
                    const uint8_t *lb = b + (g->h - 1) * stride;
                    int a1 = x & 1 ? lb[x / 2] & 15 : lb[x / 2] >> 4;
                    top |= a0 > 0;
                    bottom |= a1 > 0;
                }
                for (int y = 0; y < g->h; y++) {
                    const uint8_t *row = b + y * stride;
                    int xr = g->w - 1;
                    left |= (row[0] >> 4) > 0;
                    right |= (xr & 1 ? row[xr / 2] & 15 : row[xr / 2] >> 4) > 0;
                }
                if (!(top && bottom && left && right)) inside = false;
                /* An odd width's pad nibble is zero. */
                if (g->w & 1)
                    for (int y = 0; y < g->h; y++)
                        if (b[y * stride + stride - 1] & 15) inside = false;
            }
        }
        for (int i = 0; i < f->kern_count; i++) {
            const aafont_kern_t *q = &f->kerns[i];
            if (q->left >= f->count || q->right >= f->count || q->dx == 0) kerns_ok = false;
            if (i > 0) {
                unsigned a = (unsigned)f->kerns[i - 1].left << 8 | f->kerns[i - 1].right;
                unsigned b = (unsigned)q->left << 8 | q->right;
                if (b <= a) kerns_ok = false;
            }
        }
        for (const char *m = s_faces[k].must; *m; m++)
            if (glyph_of(f, (unsigned char)*m) == NULL) has_all = false;
        const aafont_glyph_t *zero = glyph_of(f, '0');
        for (char d = '0'; d <= '9'; d++) {
            const aafont_glyph_t *g = glyph_of(f, (unsigned char)d);
            if (zero == NULL || g == NULL || g->adv != zero->adv) tabular = false;
        }
        const aafont_glyph_t *H = glyph_of(f, 'H');

        snprintf(what, sizeof what, "%s: glyphs sorted by code point", s_faces[k].name);
        expect(what, sorted);
        snprintf(what, sizeof what, "%s: every bitmap inside bits[], cropped to its ink", s_faces[k].name);
        expect(what, inside);
        snprintf(what, sizeof what, "%s: kerning sorted, in range, no zeros", s_faces[k].name);
        expect(what, kerns_ok);
        snprintf(what, sizeof what, "%s: has all of \"%s\"", s_faces[k].name, s_faces[k].must);
        expect(what, has_all);
        snprintf(what, sizeof what, "%s: has the degree sign", s_faces[k].name);
        expect(what, glyph_of(f, 0xB0) != NULL);
        if (s_faces[k].tabular) {
            snprintf(what, sizeof what, "%s: figures are tabular", s_faces[k].name);
            expect(what, tabular);
        } else {
            snprintf(what, sizeof what, "%s: figures are proportional (a 1 narrower than a 0)", s_faces[k].name);
            expect(what, !tabular && glyph_of(f, '1')->adv < zero->adv);
        }
        snprintf(what, sizeof what, "%s: cap %d is its H, standing on the baseline", s_faces[k].name, f->cap);
        expect(what, H == NULL ? f->cap > 0 : (H->y == -f->cap && H->h == f->cap));
        snprintf(what, sizeof what, "%s: line = ascent + descent", s_faces[k].name);
        expect(what, f->line == f->ascent + f->descent && f->ascent >= f->cap);
    }
    expect("inter_label has the minus sign, and it is not the hyphen",
           glyph_of(&aafont_inter_label, 0x2212) != NULL
           && glyph_of(&aafont_inter_label, 0x2212)->w > glyph_of(&aafont_inter_label, '-')->w);

    /* The knock-out faces: P, O and R, the capitals of the face each stands
       in for to the pixel, and a weight heavier -- more ink in every letter,
       not merely a wider one. */
    for (int k = 0; k < KNOCKS; k++) {
        const aafont_t *f = s_knocks[k].f, *b = s_knocks[k].base;
        bool has = f->count == 3, heavier = true;
        for (const char *m = "POR"; *m; m++) {
            const aafont_glyph_t *g = glyph_of(f, (unsigned char)*m), *h = glyph_of(b, (unsigned char)*m);
            if (g == NULL || h == NULL) { has = false; continue; }
            long ink[2] = { 0, 0 };
            for (int w = 0; w < 2; w++) {
                const aafont_t *ff = w ? b : f;
                const aafont_glyph_t *gg = w ? h : g;
                int stride = (gg->w + 1) / 2;
                for (int y = 0; y < gg->h; y++)
                    for (int x = 0; x < gg->w; x++) {
                        uint8_t v = ff->bits[gg->off + y * stride + x / 2];
                        ink[w] += x & 1 ? v & 15 : v >> 4;
                    }
            }
            if (ink[0] * 100 < ink[1] * 108) heavier = false;     /* 8 % more at least */
        }
        snprintf(what, sizeof what, "%s: P, O and R alone", s_knocks[k].name);
        expect(what, has);
        const char *bn = "?";
        for (int j = 0; j < FACES; j++) if (s_faces[j].f == b) bn = s_faces[j].name;
        snprintf(what, sizeof what, "%s: capitals %d, as %s's", s_knocks[k].name, f->cap, bn);
        expect(what, has && f->cap == b->cap && glyph_of(f, 'P')->y == -f->cap && glyph_of(f, 'P')->h == f->cap);
        snprintf(what, sizeof what, "%s: a weight heavier than %s", s_knocks[k].name, bn);
        expect(what, heavier);
    }
}

/* ---- the blend --------------------------------------------------------------- */

static double lin(unsigned code, unsigned max) { return pow((double)code / max, 2.2); }

/* The reference: the two lights mixed, the result encoded and rounded in
   the encoded scale. */
static unsigned ref_mix(unsigned d, unsigned s, unsigned a, unsigned max)
{
    double l = lin(d, max) * (15 - a) / 15.0 + lin(s, max) * a / 15.0;
    return (unsigned)floor(pow(l, 1 / 2.2) * max + 0.5);
}

static void test_blend(void)
{
    /* Every channel pair at every coverage, red (5 bits) and green (6). */
    long exact = 0, off1 = 0, worse = 0;
    for (unsigned a = 0; a <= 15; a++)
        for (unsigned d = 0; d < 64; d++)
            for (unsigned s = 0; s < 64; s++) {
                uint16_t got = aafont_blend((uint16_t)(d << 5), (uint16_t)(s << 5), a);
                unsigned g = (got >> 5) & 63, want = ref_mix(d, s, a, 63);
                int diff = (int)g - (int)want;
                if (diff == 0) exact++; else if (diff == 1 || diff == -1) off1++; else worse++;
                if (d < 32 && s < 32) {
                    got = aafont_blend((uint16_t)(d << 11), (uint16_t)(s << 11), a);
                    g = got >> 11;
                    want = ref_mix(d, s, a, 31);
                    diff = (int)g - (int)want;
                    if (diff == 0) exact++; else if (diff == 1 || diff == -1) off1++; else worse++;
                }
            }
    printf("     blend: %ld exact, %ld one code off (ties in the tables' rounding), %ld worse\n",
           exact, off1, worse);
    expect("blend matches linear-light arithmetic, never more than a code out", worse == 0);
    expect("blend is exact but for the odd tie", off1 * 1000 < exact);

    expect("coverage 0 keeps the pixel", aafont_blend(0x1234, WHITE, 0) == 0x1234);
    expect("coverage 15 is the colour", aafont_blend(0x1234, FAIR, 15) == FAIR);
    /* Half-covered white on black: half the light, 0.5^(1/2.2) = 73 % of
       full code -- not the 50 % an average of codes would give. */
    uint16_t half = aafont_blend(BLACK, WHITE, 8);
    unsigned r = half >> 11;
    printf("     white at 8/15 over black: red code %u of 31\n", r);
    expect("half coverage is half the light", r >= 23 && r <= 24);
    bool mono = true;
    for (unsigned a = 1; a <= 15; a++)
        if (aafont_blend(BLACK, GREY, a) < aafont_blend(BLACK, GREY, a - 1)) mono = false;
    expect("more coverage is never darker", mono);
    /* Black text on the POOR block thins rather than spreads: at half cover
       the red keeps more than half its code. */
    expect("black on red keeps its red at half cover", (aafont_blend(POOR, BLACK, 8) >> 11) > 31 / 2);
}

/* ---- bounds -------------------------------------------------------------- */

#define W 320
#define H 172
#define GUARD 64
#define CANARY 0xA5C3

static uint16_t s_mem[GUARD + W * H + GUARD];

static canvas_t canvas_on(uint16_t *mem, int w, int h)
{
    canvas_t c;
    for (int i = 0; i < GUARD + w * h + GUARD; i++) mem[i] = CANARY;
    canvas_init(&c, mem + GUARD, w, h, 1);
    return c;
}

static bool guard_ok(const uint16_t *mem, int w, int h)
{
    for (int i = 0; i < GUARD; i++)
        if (mem[i] != CANARY || mem[GUARD + w * h + i] != CANARY) return false;
    return true;
}

static void test_bounds(void)
{
    static const char *texts[] = {
        "HUMIDITY 24H", "-24H NOW", ("22.5" DEG), "21:47:09", "Wg@|}", "POOR",
        "\xB0\xC2", "\xE2\x28\xA1", "\xF4\x90\x80\x80 x", "\xED\xA0\x80", "\xC0\xAF", "A\xC2",
        "", "   ",
    };
    static const int aligns[] = {
        AAFONT_LEFT, AAFONT_CENTRE, AAFONT_RIGHT,
        AAFONT_LEFT | AAFONT_ADVANCE, AAFONT_CENTRE | AAFONT_ADVANCE, AAFONT_RIGHT | AAFONT_ADVANCE,
        99, -1,
    };
    static const int sizes[][2] = { { W, H }, { 1, 1 }, { 3, 2 }, { 7, 40 } };
    bool ok = true;
    long draws = 0;
    for (size_t z = 0; z < sizeof sizes / sizeof sizes[0]; z++) {
        int w = sizes[z][0], h = sizes[z][1];
        canvas_t c = canvas_on(s_mem, w, h);
        for (int k = 0; k < FACES; k++)
            for (size_t t = 0; t < sizeof texts / sizeof texts[0]; t++)
                for (size_t a = 0; a < sizeof aligns / sizeof aligns[0]; a++)
                    for (int y = -90; y <= h + 90; y += 13)
                        for (int x = -350; x <= w + 350; x += 23) {
                            aafont_draw(&c, s_faces[k].f, x, y, texts[t], WHITE, aligns[a]);
                            draws++;
                        }
        ok = ok && guard_ok(s_mem, w, h);
    }
    printf("     %ld draws\n", draws);
    expect("no draw writes outside the framebuffer, on any canvas size", ok);

    canvas_t c = canvas_on(s_mem, W, H);
    static const int far[] = { INT_MIN, INT_MIN + 1, -1000000, 1000000, INT_MAX - 1, INT_MAX };
    for (int k = 0; k < FACES; k++)
        for (size_t i = 0; i < sizeof far / sizeof far[0]; i++)
            for (size_t j = 0; j < sizeof far / sizeof far[0]; j++)
                for (size_t a = 0; a < sizeof aligns / sizeof aligns[0]; a++) {
                    aafont_draw(&c, s_faces[k].f, far[i], far[j], "88:88", WHITE, aligns[a]);
                    aafont_draw(&c, s_faces[k].f, far[i], 50, "88:88", WHITE, aligns[a]);
                    aafont_draw(&c, s_faces[k].f, 50, far[j], "88:88", WHITE, aligns[a]);
                }
    bool clean = true;
    for (int i = 0; i < W * H; i++) if (c.fb[i] != CANARY) clean = false;
    expect("text placed at the ends of int neither crashes nor lands on the panel", clean && guard_ok(s_mem, W, H));

    /* A long string: the pen is 64-bit, and the width is capped, not wrapped. */
    static char longs[200001];
    memset(longs, '8', sizeof longs - 1);
    longs[sizeof longs - 1] = '\0';
    int lw = aafont_width(&aafont_inter_number, longs);
    aafont_draw(&c, &aafont_inter_number, 0, 50, longs, WHITE, AAFONT_RIGHT);
    expect("a 200 000 figure number measures sanely and stays inside", lw > 0 && guard_ok(s_mem, W, H));

    canvas_t none = c;
    none.fb = NULL;
    expect("NULL canvas, framebuffer, font or text draws nothing",
           aafont_draw(NULL, &aafont_inter_label, 0, 0, "A", WHITE, 0) == 0
           && aafont_draw(&none, &aafont_inter_label, 0, 0, "A", WHITE, 0) == 0
           && aafont_draw(&c, NULL, 0, 0, "A", WHITE, 0) == 0
           && aafont_draw(&c, &aafont_inter_label, 0, 0, NULL, WHITE, 0) == 0
           && aafont_width(NULL, "A") == 0 && aafont_width(&aafont_inter_label, NULL) == 0
           && aafont_advance(NULL, "A") == 0 && guard_ok(s_mem, W, H));
}

/* ---- where the ink goes --------------------------------------------------------- */

#define BW 1400
#define BH 220
static uint16_t s_big[BW * BH];

typedef struct { int l, r, t, b; bool any; } box_t;   /* inclusive */

static box_t ink_box(const uint16_t *fb, int w, int h)
{
    box_t b = { w, -1, h, -1, false };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (fb[y * w + x] != BLACK) {
                if (x < b.l) b.l = x;
                if (x > b.r) b.r = x;
                if (y < b.t) b.t = y;
                if (y > b.b) b.b = y;
                b.any = true;
            }
    return b;
}

static box_t draw_box(const aafont_t *f, int x, int y, const char *s, int align, int *ret)
{
    canvas_t c;
    canvas_init(&c, s_big, BW, BH, 1);
    canvas_clear(&c);
    *ret = aafont_draw(&c, f, x, y, s, WHITE, align);
    return ink_box(s_big, BW, BH);
}

static void test_placement(void)
{
    static const char *texts[] = {
        "VOC", "ppb", "eCO2 est", "-24H", "NOW", "HUMIDITY 24H", "1100", ("22.5" DEG),
        ("-5.2" DEG), "37%", "21:47:09", "WARMING UP", "AIR GOOD", "  FAIR  ", "Wy", "1", ".",
    };
    char what[200];
    for (int k = 0; k < FACES; k++) {
        const aafont_t *f = s_faces[k].f;
        bool width_ok = true, left_ok = true, right_ok = true, centre_ok = true, ret_ok = true;
        bool top_ok = true, adv_ok = true;
        int n = 0;
        /* The sample texts, and every glyph alone. */
        for (int i = 0; i < (int)(sizeof texts / sizeof texts[0]) + f->count; i++) {
            char one[8];
            const char *s;
            if (i < (int)(sizeof texts / sizeof texts[0])) s = texts[i];
            else {
                uint16_t cp = f->glyphs[i - (int)(sizeof texts / sizeof texts[0])].cp;
                utf8_of(cp, one);
                s = one;
            }
            int w = aafont_width(f, s), ret;
            if (w == 0) continue;                     /* a space, or a character it lacks */
            n++;
            box_t b = draw_box(f, 300, 100, s, AAFONT_LEFT, &ret);
            if (!b.any || b.r - b.l + 1 != w) width_ok = false;
            if (b.l != 300) left_ok = false;
            if (ret != w) ret_ok = false;
            b = draw_box(f, 1000, 100, s, AAFONT_RIGHT, &ret);
            if (b.r != 999) right_ok = false;
            b = draw_box(f, 700, 100, s, AAFONT_CENTRE, &ret);
            if (abs((b.l + b.r + 1) - 2 * 700) > 1) centre_ok = false;

            /* The advance box: its left edge on x, its right edge, its middle. */
            const aafont_glyph_t *g0 = NULL;
            {
                /* The first inked glyph's offset from the pen, when it is the
                   first glyph of all (so no kerning and no pen moved yet). */
                const unsigned char *p = (const unsigned char *)s;
                if (*p >= 0x21 && *p < 0x80) g0 = glyph_of(f, *p);
            }
            if (g0 != NULL && g0->w > 0) {
                if (aafont_bearing(f, s) != g0->x) adv_ok = false;
                int adv = aafont_advance(f, s);
                b = draw_box(f, 300, 100, s, AAFONT_LEFT | AAFONT_ADVANCE, &ret);
                if (b.l != 300 + g0->x) adv_ok = false;
                b = draw_box(f, 1000, 100, s, AAFONT_RIGHT | AAFONT_ADVANCE, &ret);
                if (b.l != 1000 - adv + g0->x) adv_ok = false;
                b = draw_box(f, 700, 100, s, AAFONT_CENTRE | AAFONT_ADVANCE, &ret);
                if (b.l != 700 - adv / 2 + g0->x) adv_ok = false;
            }
        }
        /* y is the capitals' top: an H's first row is y, its last y + cap - 1. */
        int ret;
        box_t b = draw_box(f, 300, 100, glyph_of(f, 'H') ? "H" : "8", AAFONT_LEFT, &ret);
        if (glyph_of(f, 'H') ? (b.t != 100 || b.b != 100 + f->cap - 1) : (abs(b.t - 100) > 1)) top_ok = false;

        snprintf(what, sizeof what, "%s: aafont_width is the drawn ink, %d texts and glyphs", s_faces[k].name, n);
        expect(what, width_ok && ret_ok);
        snprintf(what, sizeof what, "%s: ink left / right / centre on x", s_faces[k].name);
        expect(what, left_ok && right_ok && centre_ok);
        snprintf(what, sizeof what, "%s: the advance box left / right / centre on x", s_faces[k].name);
        expect(what, adv_ok);
        snprintf(what, sizeof what, "%s: y is the top of the capitals", s_faces[k].name);
        expect(what, top_ok);
    }

    /* Tabular: a clock's advance does not change with its digits, so aligned
       by it, it does not move. */
    const aafont_t *n = &aafont_inter_number;
    expect("the clock's advance box is the same for every time",
           aafont_advance(n, "11:11:11") == aafont_advance(n, "00:00:00")
           && aafont_advance(n, "21:47:09") == aafont_advance(n, "88:88:88"));
    /* Centred on the panel by that box, the ink stays inside x 16..304 for
       the widest time there is. */
    printf("     HH:MM:SS in inter_number: %d px of advance, %d of ink\n",
           aafont_advance(n, "00:00:00"), aafont_width(n, "00:00:00"));
    bool fits = true;
    static const char *times[] = { "00:00:00", "08:08:08", "11:11:11", "21:47:09", "23:59:59" };
    for (size_t i = 0; i < sizeof times / sizeof times[0]; i++) {
        int ret;
        box_t b = draw_box(n, 160, 50, times[i], AAFONT_CENTRE | AAFONT_ADVANCE, &ret);
        if (b.l < 16 || b.r >= 304) fits = false;
    }
    expect("HH:MM:SS centred by its advance keeps inside envo's margins", fits);

    /* Kerning is applied: AV and FA close up in Inter. */
    const aafont_t *wd = &aafont_inter_word;
    expect("kerning closes AV", aafont_advance(wd, "AV") < aafont_advance(wd, "A") + aafont_advance(wd, "V"));
    expect("kerning closes FA", aafont_advance(wd, "FA") < aafont_advance(wd, "F") + aafont_advance(wd, "A"));

    /* Text. */
    expect("the degree sign in UTF-8 and as a stray Latin-1 byte draw alike",
           aafont_width(&aafont_inter_label, "25" DEG) == aafont_width(&aafont_inter_label, "25\xB0")
           && aafont_width(&aafont_inter_label, DEG) > 0);
    expect("lower case in a capitals face comes out as capitals",
           aafont_width(wd, "warm up") == aafont_width(wd, "WARM UP"));
    expect("but the word faces' own e and s are lower case, x-height tall",
           glyph_of(wd, 'e') != NULL && glyph_of(wd, 's') != NULL
           && glyph_of(wd, 'e')->h < glyph_of(wd, 'E')->h && glyph_of(wd, 's')->h < glyph_of(wd, 'S')->h);
    expect("the bearing of nothing, or of spaces, is 0",
           aafont_bearing(wd, "") == 0 && aafont_bearing(wd, "   ") == 0 && aafont_bearing(NULL, "A") == 0);
    expect("a leading space moves the bearing by its advance",
           aafont_bearing(wd, " A") == aafont_advance(wd, " ") + aafont_bearing(wd, "A"));
    expect("a character the face lacks advances like a space",
           aafont_advance(wd, "A~B") == aafont_advance(wd, "A B")
           && aafont_width(wd, "~") == 0 && aafont_advance(wd, "~") == aafont_advance(wd, " "));
}

/* ---- the specimen ------------------------------------------------------------ */

#define SW 700
#define SH 1760
static uint16_t s_sheet[SW * SH];

static void put16(FILE *f, unsigned v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
static void put32(FILE *f, unsigned long v)
{
    put16(f, (unsigned)(v & 0xFFFF));
    put16(f, (unsigned)((v >> 16) & 0xFFFF));
}

/* A 24-bit BMP of an RGB565 image, each pixel repeated `zoom` times each
   way. 565 goes to 888 by repeating the top bits into the bottom, so white
   stays 255 and black 0. */
static int write_bmp(const char *path, const uint16_t *fb, int w, int h, int zoom)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) return 0;
    int ow = w * zoom, oh = h * zoom;
    int stride = (ow * 3 + 3) & ~3;
    unsigned long size = 54ul + (unsigned long)stride * (unsigned long)oh;
    fputc('B', f); fputc('M', f);
    put32(f, size); put32(f, 0); put32(f, 54);
    put32(f, 40); put32(f, (unsigned long)ow); put32(f, (unsigned long)oh);
    put16(f, 1); put16(f, 24); put32(f, 0);
    put32(f, (unsigned long)stride * (unsigned long)oh);
    put32(f, 2835); put32(f, 2835); put32(f, 0); put32(f, 0);
    for (int y = oh - 1; y >= 0; y--) {        /* bottom-up */
        int n = 0;
        for (int x = 0; x < ow; x++) {
            uint16_t p = fb[(y / zoom) * w + x / zoom];
            unsigned r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
            fputc((int)((b << 3) | (b >> 2)), f);
            fputc((int)((g << 2) | (g >> 4)), f);
            fputc((int)((r << 3) | (r >> 2)), f);
            n += 3;
        }
        while (n < stride) { fputc(0, f); n++; }
    }
    fclose(f);
    return 1;
}

/* Every glyph of a face, in code point order, wrapped to the sheet. Returns
   the y under the last line. */
static int charset_lines(canvas_t *c, const aafont_t *f, int y, uint16_t col)
{
    char line[400] = "";
    size_t n = 0;
    int step = f->line + 2;
    for (int i = 0; i < f->count; i++) {
        uint16_t cp = f->glyphs[i].cp;
        if (cp == ' ') continue;
        char one[4];
        utf8_of(cp, one);
        char trial[420];
        snprintf(trial, sizeof trial, "%s%s", line, one);
        if (n > 0 && aafont_advance(f, trial) > SW - 24) {
            aafont_draw(c, f, 12, y, line, col, AAFONT_LEFT | AAFONT_ADVANCE);
            y += step;
            n = 0;
            line[0] = '\0';
        }
        n += (size_t)snprintf(line + n, sizeof line - n, "%s ", one);
    }
    if (n > 0) {
        aafont_draw(c, f, 12, y, line, col, AAFONT_LEFT | AAFONT_ADVANCE);
        y += step;
    }
    return y;
}

/* A POOR block as envui sets it after a white word: the knock-out face on a
   block `pad` past its ink. */
static void pw_knock_demo(canvas_t *c, const aafont_t *k, int x, int y, int pad)
{
    int pw = aafont_width(k, "POOR");
    canvas_fill_rect(c, x, y - pad, pw + 2 * pad, k->cap + 2 * pad, POOR);
    aafont_draw(c, k, x + pad, y, "POOR", BLACK, AAFONT_LEFT);
}

static void frame(canvas_t *c, int x, int y, int w, int h)
{
    canvas_fill_rect(c, x - 1, y - 1, w + 2, 1, RULE);
    canvas_fill_rect(c, x - 1, y + h, w + 2, 1, RULE);
    canvas_fill_rect(c, x - 1, y, 1, h, RULE);
    canvas_fill_rect(c, x + w, y, 1, h, RULE);
}

/* The reading page and the clock at envo's size, with each face where the
   rewired envui would put it, to judge the sizes at 1x. */
static void mock_pages(canvas_t *c, int ox, int oy)
{
    canvas_t p;
    const aafont_t *L = &aafont_inter_label, *S = &aafont_inter_sub, *Wd = &aafont_inter_word,
                   *St = &aafont_inter_state, *Nm = &aafont_inter_number;

    /* A reading page: label and unit, the number and its state, the trend. */
    frame(c, ox, oy, 320, 172);
    p = *c;
    aafont_draw(&p, L, ox + 16, oy + 8, "VOC", GREY, AAFONT_LEFT);
    aafont_draw(&p, L, ox + 304, oy + 8, "ppb", GREY, AAFONT_RIGHT);
    aafont_draw(&p, Nm, ox + 16, oy + 90 - Nm->cap, "260", WHITE, AAFONT_LEFT);
    aafont_draw(&p, St, ox + 16, oy + 98, "FAIR", FAIR, AAFONT_LEFT);
    aafont_draw(&p, S, ox + 304, oy + 90 - S->cap, "RISING", WHITE, AAFONT_RIGHT);
    canvas_fill_rect(&p, ox + 16, oy + 152, 288, 1, RULE);
    aafont_draw(&p, L, ox + 16, oy + 156, MINUS "24H", GREY, AAFONT_LEFT);
    aafont_draw(&p, L, ox + 304, oy + 156, "NOW", GREY, AAFONT_RIGHT);

    /* The temperature, POOR's block in the knock-out weight, and humidity's
       percent; the week's days and hours along the foot. */
    int x2 = ox + 330;
    frame(c, x2, oy, 320, 172);
    aafont_draw(&p, L, x2 + 16, oy + 8, "TEMP", GREY, AAFONT_LEFT);
    aafont_draw(&p, L, x2 + 304, oy + 8, DEG "C", GREY, AAFONT_RIGHT);
    aafont_draw(&p, Nm, x2 + 16, oy + 90 - Nm->cap, "-5.2" DEG, WHITE, AAFONT_LEFT);
    const aafont_t *K = &aafont_inter_state_knock;
    int pw = aafont_width(K, "POOR");
    canvas_fill_rect(&p, x2 + 12, oy + 94, pw + 8, St->cap + 8, POOR);
    aafont_draw(&p, K, x2 + 16, oy + 98, "POOR", BLACK, AAFONT_LEFT);
    aafont_draw(&p, S, x2 + 304, oy + 90 - S->cap, "STEADY", GREY, AAFONT_RIGHT);
    aafont_draw(&p, Wd, x2 + 304, oy + 98 + St->cap - Wd->cap, "37%", WHITE, AAFONT_RIGHT);
    aafont_draw(&p, L, x2 + 16, oy + 150, "Mo Tu We Th Fr Sa Su", GREY, AAFONT_LEFT);
    aafont_draw(&p, L, x2 + 304, oy + 150, "06 12 18", GREY, AAFONT_RIGHT);
    expect("the specimen's temperature clears STEADY",
           x2 + 16 + aafont_width(Nm, "-5.2" DEG) + 10 <= x2 + 304 - aafont_width(S, "STEADY"));
    expect("the specimen's days and hours fit their panel",
           aafont_width(L, "Mo Tu We Th Fr Sa Su") + 12 + aafont_width(L, "06 12 18") <= 288);

    /* The clock: date, HH:MM:SS centred by its advance, the verdict. */
    int y3 = oy + 182;
    frame(c, ox, y3, 320, 172);
    aafont_draw(&p, S, ox + 160, y3 + 26, "Thu 25 Sep", WHITE, AAFONT_CENTRE);
    aafont_draw(&p, Nm, ox + 160, y3 + 58, "21:47:09", WHITE, AAFONT_CENTRE | AAFONT_ADVANCE);
    int aw = aafont_width(St, "AIR"), gw = aafont_width(St, "GOOD"), gap = 12;
    int vx = ox + 160 - (aw + gap + gw) / 2;
    aafont_draw(&p, St, vx, y3 + 122, "AIR", WHITE, AAFONT_LEFT);
    aafont_draw(&p, St, vx + aw + gap, y3 + 122, "GOOD", GOOD, AAFONT_LEFT);

    /* The chart header and key. */
    frame(c, x2, y3, 320, 172);
    aafont_draw(&p, L, x2 + 16, y3 + 8, "VOC 24H", WHITE, AAFONT_LEFT);
    aafont_draw(&p, L, x2 + 16, y3 + 26, "ppb", GREY, AAFONT_LEFT);
    aafont_draw(&p, Wd, x2 + 304, y3 + 14, "GOOD", GOOD, AAFONT_RIGHT);
    aafont_draw(&p, St, x2 + 304 - aafont_width(Wd, "GOOD") - 10, y3 + 40 - St->cap, "45", WHITE, AAFONT_RIGHT);
    /* The key as envui lays it out: POOR at the plot's top, each line's value
       level with it, FAIR between them, GOOD under. */
    int yp = y3 + 68, yf = y3 + 100;
    canvas_fill_rect(&p, x2 + 16, y3 + 142, 228, 1, RULE);
    canvas_fill_rect(&p, x2 + 16, yp, 228, 1, POOR);
    canvas_fill_rect(&p, x2 + 16, yf, 228, 1, FAIR);
    int half = L->cap / 2;
    aafont_draw(&p, L, x2 + 253, y3 + 45, "POOR", POOR, AAFONT_LEFT);
    aafont_draw(&p, L, x2 + 253, yp - half, "2200", GREY, AAFONT_LEFT);
    aafont_draw(&p, L, x2 + 253, (yp + yf) / 2 - half, "FAIR", FAIR, AAFONT_LEFT);
    aafont_draw(&p, L, x2 + 253, yf - half, "650", GREY, AAFONT_LEFT);
    aafont_draw(&p, L, x2 + 253, yf + 14, "GOOD", GOOD, AAFONT_LEFT);
    expect("the key's words end on the margin", aafont_width(L, "GOOD") <= 304 - 253
           && aafont_width(L, "POOR") <= 304 - 253);
    aafont_draw(&p, L, x2 + 16, y3 + 150, "18", GREY, AAFONT_LEFT);
    aafont_draw(&p, L, x2 + 90, y3 + 150, "FRI", WHITE, AAFONT_LEFT);
    aafont_draw(&p, L, x2 + 244, y3 + 150, "NOW", WHITE, AAFONT_RIGHT);
}

static void test_specimen(void)
{
    canvas_t c;
    canvas_init(&c, s_sheet, SW, SH, 1);
    canvas_clear(&c);
    const aafont_t *L = &aafont_inter_label;
    int y = 10;
    char cap[160];
    for (int k = 0; k < FACES; k++) {
        const aafont_t *f = s_faces[k].f;
        snprintf(cap, sizeof cap, "%s: %s, capitals %d, x-height %d, %d glyphs, %d kerns",
                 s_faces[k].name, s_faces[k].face, f->cap, f->x_height, f->count, f->kern_count);
        aafont_draw(&c, L, 12, y, cap, GREY, AAFONT_LEFT);
        y += L->line + 6;
        y = charset_lines(&c, f, y, WHITE);
        y += 4;
    }
    /* The knock-out faces, black on the red they are for, beside the face
       each stands in for. */
    for (int k = 0; k < KNOCKS; k++) {
        const aafont_t *f = s_knocks[k].f, *b = s_knocks[k].base;
        const char *bn = "?";
        for (int j = 0; j < FACES; j++) if (s_faces[j].f == b) bn = s_faces[j].name;
        snprintf(cap, sizeof cap, "%s: %s, capitals %d -- beside %s", s_knocks[k].name, s_knocks[k].face,
                 f->cap, bn);
        aafont_draw(&c, L, 12, y, cap, GREY, AAFONT_LEFT);
        y += L->line + 6;
        int pad = (f->cap + 4) / 8, x = 12;
        for (int w = 0; w < 2; w++) {
            const aafont_t *ff = w ? b : f;
            int pw = aafont_width(ff, "POOR");
            canvas_fill_rect(&c, x, y - pad, pw + 2 * pad, f->cap + 2 * pad, POOR);
            x += pad;
            x += aafont_draw(&c, ff, x, y, "POOR", BLACK, AAFONT_LEFT) + pad + 16;
        }
        x += aafont_draw(&c, b, x, y, "VOC", WHITE, AAFONT_LEFT) + aafont_advance(b, " ");
        pw_knock_demo(&c, f, x, y, pad);
        y += f->cap + 2 * pad + 8;
    }
    /* The words in envo's colours, on black and on the blocks they sit on. */
    const aafont_t *St = &aafont_inter_state, *Wd = &aafont_inter_word;
    int x = 12;
    x += aafont_draw(&c, St, x, y, "GOOD", GOOD, AAFONT_LEFT) + 18;
    x += aafont_draw(&c, St, x, y, "FAIR", FAIR, AAFONT_LEFT) + 18;
    const aafont_t *K = &aafont_inter_state_knock;
    int pw = aafont_width(K, "POOR");
    canvas_fill_rect(&c, x - 4, y - 4, pw + 8, St->cap + 8, POOR);
    x += aafont_draw(&c, K, x, y, "POOR", BLACK, AAFONT_LEFT) + 22;
    aafont_draw(&c, Wd, x, y + St->cap - Wd->cap, "WARMING UP", GREY, AAFONT_LEFT);
    y += St->cap + 16;
    canvas_fill_rect(&c, 0, y - 6, SW, Wd->cap + 12, WHITE);
    int dw = aafont_draw(&c, Wd, 12, y, "DARK ON LIGHT -5.2" DEG, BLACK, AAFONT_LEFT);
    aafont_draw(&c, L, 12 + dw + 16, y + Wd->cap - L->cap, "eCO2 est  ppm, est.  " MINUS "24H", BLACK, AAFONT_LEFT);
    y += Wd->cap + 18;

    mock_pages(&c, 4, y);

    mkdir("renders", 0755);
    mkdir("renders/aafont", 0755);
    printf("     specimen: %d of %d rows used\n", y + 182 + 172, SH);
    expect("specimen fits its sheet", y + 182 + 172 < SH);
    expect("render renders/aafont/specimen.bmp",
           write_bmp("renders/aafont/specimen.bmp", s_sheet, SW, SH, 1)
           && write_bmp("renders/aafont/specimen_3x.bmp", s_sheet, SW, SH, 3));
}

int main(void)
{
    test_tables();
    test_blend();
    test_bounds();
    test_placement();
    test_specimen();

    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("all passed\n");
    return 0;
}
