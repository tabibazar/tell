#include "envui.h"

#include "aafont.h"
#include "vector.h"

#include <math.h>
#include <stdio.h>

/*
 * The layouts are the spec's, measured off the research mockups
 * (docs/design/envo-ui/mock_rev1.c page_single, mock_rev2.c for the chart and
 * the week). Every piece of text is Inter, anti-aliased (aafont.h), and every
 * y below is the top of its capitals' ink, so two pieces said to line up do,
 * whichever size each is. Where text of two sizes shares a line, the line is
 * a baseline -- the capitals' top plus the face's cap -- the way type is set.
 */

#define N ENVUI_SLOTS

/*
 * Colour means a state and nothing else, and always comes with its word.
 * Contrast on black, from the research: grey labels 5.5:1, hairlines 3:1,
 * blue 6.8:1, amber 11:1. Red is only ever a block with black text on it,
 * or a label in the key naming it.
 */
#define COL_BG     0x0000
#define COL_WHITE  0xFFFF
#define COL_GREY   0x8410   /* labels */
#define COL_RULE   0x5ACB   /* hairlines, the floor, gap hatching */
#define COL_GOOD   0x04BF
#define COL_FAIR   0xFD40
#define COL_POOR   0xF8C1
#define COL_TINT   0x00E7   /* navy under the GOOD zone, VOC only */

/* Text stays within x 16..304: the panel's corners are rounded, and a
   letter at x 4 is a letter half cut off. */
#define XL 16
#define XR 304

/*
 * The faces, by job (aafont.h has what each is and why): six sizes, four
 * ranks. Labels in the smallest; a secondary line one up; the words that
 * name a state in the next two; the numbers in the largest two.
 */
#define F_LABEL   (&aafont_inter_label)     /* capitals 13: labels, units, axis, key, week */
#define F_SUB     (&aafont_inter_sub)       /* 17: the trend word, minutes so far, the date */
#define F_WORD    (&aafont_inter_word)      /* 25: FROM VOCs, NO READING, the chart's word */
#define F_STATE   (&aafont_inter_state)     /* 32: GOOD / FAIR / POOR, the chart's number */
#define F_NUMBER  (&aafont_inter_number)    /* 47: the clock's time */
#define F_READING (&aafont_inter_reading)   /* 57: the reading */

#define DEG "\xC2\xB0"
#define MINUS "\xE2\x88\x92"                 /* U+2212, in F_LABEL */

/*
 * The POOR block's word, a weight heavier than the face it stands in for:
 * black on red reads lighter than white on black at the same weight, and at
 * the same weight the word on the block looked a size smaller than the white
 * one beside it (aafont.h). Same capitals, so it stands on the same line.
 */
static const aafont_t *knockout(const aafont_t *f)
{
    return f == F_LABEL ? &aafont_inter_label_knock : f == F_SUB ? &aafont_inter_sub_knock
         : f == F_WORD ? &aafont_inter_word_knock : f == F_STATE ? &aafont_inter_state_knock : f;
}

/* ---- the reading page -------------------------------------------------- */

#define R_LABEL_TOP  8
/*
 * The number stands on a fixed baseline, so when a wide one steps down a
 * size it stays on the same line above its word. At full size its capitals
 * are rows 33..89, 57 px, where the stroke number's ink was 63.
 */
#define R_NUM_BASE   90
#define R_NUM_MAXW   200
#define R_NUM_GAP    10     /* between the number and the trend word */
#define R_SUFFIX_GAP 4      /* a degree or percent sign above the word: kept off it sideways too */
/* The state word under it, capitals 98..129 and baseline 130; POOR's block
   reaches 4 px above and below, 94..133, clear of the number above and the
   strip below, and 6 px either side (block_side). FROM VOCs and NO READING, a
   size smaller, stand on the same baseline. */
#define R_WORD_TOP   98
#define R_WORD_BASE  130
#define R_WORD_PAD   4
/* The trend, right-aligned: the arrow at the top of the number's band, the
   word standing on the number's baseline. */
#define R_ARROW      26.0f
/* The 24 h strip, rows 138..151 over a floor at 152, then its end labels
   with the page dots between them, on the labels' middle. */
#define R_STRIP_FLOOR 152
#define R_STRIP_H     14.0f
#define R_STRIP_LABEL 156
#define R_DOTS_Y      162.5f

/* ---- the full chart ------------------------------------------------------ */

/* The plot ends 10 px short of the key rather than 7, so the now-dot, 3.5 px
   in radius, is not read as a bullet on the key word level with it. The key
   starts where its widest word, GOOD (51 px of ink), ends on the margin. */
#define D_X0     16
#define D_X1     243        /* 228 columns of about 6.3 minutes */
#define D_FLOOR  142        /* plot rows 44..141 */
#define D_H      98.0f
#define D_KEY_X  253
#define D_AXIS   150        /* the time axis labels */
/*
 * The header. The title's capitals start at y 8 and the unit under it stands
 * on y 40; the number on the right spans both, its capitals from 8 to that
 * same baseline, and the state word beside it stands on it too. POOR's block
 * then spans 13..41, clear of the key's POOR under it at 45, and the unit's
 * descenders end on row 43, above the plot; 4 px either side.
 */
#define D_TITLE_TOP  8
#define D_BASE       40
#define D_WORD_PAD   2
#define D_ARROW      18.0f
#define D_ARROW_CY   26.0f  /* between the number's middle and the word's */

/* ---- the week ------------------------------------------------------------ */

/*
 * Rows 16 px apart rather than the mockup's 17, so the hour labels finish at
 * y 153 and the page dots fit under them at the same height as on every
 * other page. The grid starts at x 44, a pixel right of the mockup's 43, so
 * the widest day, "We" (ink 16..41), clears a full-height cell by two
 * columns rather than one; its last column is then 305, 44 + 23 * 11 + 9 - 1,
 * a pixel past the text's margin -- a cell, not a letter, and half-way down
 * the panel, nowhere near a rounded corner.
 */
#define W_GX   44
#define W_GY   28
#define W_CW   11
#define W_CH   16
#define W_CELL_W 9
#define W_CELL_H 12

/* ---- the clock ------------------------------------------------------------- */

/*
 * The date, the time and the verdict, centred, the block of them centred on
 * the panel: the date's capitals 22..38, the time's 53..99, the verdict's
 * 118..149. The date sits close over the time, which it belongs to; the
 * verdict is a line of its own, further off.
 */
#define K_DATE_TOP     22
#define K_TIME_TOP     53
#define K_VERDICT_TOP  118

/* ---- series ---------------------------------------------------------------- */

typedef struct {
    const char *label;       /* the reading page, top left */
    const char *unit;        /* the reading page, top right */
    const char *title;       /* the full chart, top left */
    const char *subtitle;    /* the full chart, under the title */
    int32_t lo, hi;          /* the fixed axis, for a series without zones */
    int32_t ref[2];          /* reference lines, likewise */
} meta_t;

/*
 * Temperature and humidity are the AHT21's raw numbers: the gas sensor is
 * isolated and the thermometer is not assumed to read warm, so there is no
 * offset, no "est", and no comfort word until a trusted one says otherwise.
 *
 * eCO2's reading page says "eCO2 est" over a plain "ppm": the label already
 * says it is an estimate, FROM VOCs says where from in the word's slot, and
 * "ppm, from VOCs" filled the top line from edge to edge, so that label and
 * unit ran together into one phrase. Its chart is titled "eCO2 24H" over
 * "ppm, est.": the header's number and word have to clear both lines, and
 * "est." keeps the estimate named on the one page that has no label.
 *
 * A temperature's unit is "°C" now that the type has a degree sign; the
 * 12x24 font had none, and said "C".
 */
static const meta_t s_meta[ENVS_N] = {
    [ENVS_VOC]  = { "VOC",      "ppb",  "VOC 24H",      "ppb",       0,    2200, { 0, 0 } },
    [ENVS_ECO2] = { "eCO2 est", "ppm",  "eCO2 24H",     "ppm, est.", 400,  1500, { 0, 0 } },
    [ENVS_TEMP] = { "TEMP",     DEG "C", "TEMP 24H",    DEG "C",     1600, 3000, { 2000, 2500 } },
    [ENVS_RH]   = { "HUMIDITY", "%",    "HUMIDITY 24H", "%",         2000, 8000, { 3000, 6000 } },
};

/* Through int, because an enum with no negative member may be unsigned and a
   check for below zero on it is a warning that it can never be true. */
static bool series_ok(envs_series_t s) { return (int)s >= 0 && (int)s < ENVS_N; }
static bool is_gas(envs_series_t s) { return s == ENVS_VOC || s == ENVS_ECO2; }

/* The gas pages lose their number while the ENS160 warms up or errs: what it
   says then is not a reading, and must not set a word, a colour or a dot. */
static bool gas_waiting(const envui_series_t *s)
{
    return is_gas(s->series) && (s->warming || s->gas_error);
}

/* What the gas pages, the chart and the clock all say then -- one word for
   each, from one place, so no two of them can disagree. */
static const char *wait_text(bool gas_error)
{
    return gas_error ? "GAS ERROR" : "WARMING UP";
}

/*
 * The number as shown: truncated, never rounded, to a step coarse enough
 * that the last digit is not sensor noise -- VOC in 5 ppb below 100, 10 up to
 * 1000, 100 above; eCO2 in 10 ppm; temperature 0.1 C; humidity 1 %.
 * Truncated so the number never shows an edge the reading has not reached:
 * 218 ppb shows 210, not a 220 that sits on the FAIR line while the word
 * says GOOD. The same steps as envs_quantise(), kept here so envui needs
 * nothing from envstate but its types and the limits table; the state is
 * always worked out from the unrounded value, never from this.
 */
static int64_t shown(envs_series_t s, int32_t v)
{
    int64_t q;
    switch (s) {
    case ENVS_VOC:  q = v < 100 ? 5 : v < 1000 ? 10 : 100; break;
    case ENVS_ECO2: q = 10; break;
    case ENVS_TEMP: q = 10; break;           /* 0.1 C, in 0.01 C */
    case ENVS_RH:   q = 100; break;          /* 1 %, in 0.01 % */
    default:        q = 1; break;
    }
    return (int64_t)v - (int64_t)v % q;      /* towards zero */
}

static void format_value(envs_series_t s, int32_t v, char *out, size_t size)
{
    long long q = (long long)shown(s, v);
    if (s == ENVS_TEMP) {
        long long a = q < 0 ? -q : q;
        snprintf(out, size, "%s%lld.%lld", q < 0 ? "-" : "", a / 100, a % 100 / 10);
    } else if (s == ENVS_RH) {
        snprintf(out, size, "%lld", q / 100);
    } else {
        snprintf(out, size, "%lld", q);
    }
}

/*
 * Height above the floor, in px of a plot `h` tall, on the series' fixed
 * scale. Gas gets three zones, each linear inside, of fixed shares -- GOOD
 * 42, FAIR 32, POOR 24 of every 98 px -- so clean air sits low and plainly
 * GOOD, a bad hour climbs through the lines that name it, and the change of
 * slope always happens at a drawn, labelled line. Above the top is pinned.
 * Temperature and humidity are plain linear, 16..30 C and 20..80 %.
 */
static float level(envs_series_t s, int32_t v, float h)
{
    const envs_limits_t *L = is_gas(s) ? envs_limits(s) : NULL;
    float f, x = (float)v;
    if (L != NULL && L->floor < L->fair && L->fair < L->poor && L->poor < L->top) {
        if (x <= (float)L->floor)     f = 0.0f;
        else if (x < (float)L->fair)  f = (x - (float)L->floor) / (float)(L->fair - L->floor) * 42.0f;
        else if (x < (float)L->poor)  f = 42.0f + (x - (float)L->fair) / (float)(L->poor - L->fair) * 32.0f;
        else if (x < (float)L->top)   f = 74.0f + (x - (float)L->poor) / (float)(L->top - L->poor) * 24.0f;
        else                          f = 98.0f;
        return f / 98.0f * h;
    }
    const meta_t *m = &s_meta[series_ok(s) ? s : ENVS_VOC];
    f = (x - (float)m->lo) / (float)(m->hi - m->lo);
    if (!(f > 0.0f)) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f * h;
}

/*
 * Where a plot's zone edges fall, as the float heights the trace is drawn at,
 * so that a point's colour can be read off its height: white in GOOD, amber
 * in FAIR, red in POOR, from the plain limits (the trace has no memory, so no
 * hysteresis). Temperature and humidity have no zones and are white.
 */
typedef struct { float fair, poor; bool zoned; } zones_t;

static uint16_t zone_colour(const zones_t *z, float y)
{
    if (!z->zoned || y > z->fair) return COL_WHITE;
    return y > z->poor ? COL_FAIR : COL_POOR;
}

/* ---- drawing helpers ------------------------------------------------------ */

/* The first of `n` faces, largest first, that sets `s` no wider than `maxw`;
   the last, the smallest, when none does. */
static const aafont_t *fit(const aafont_t *const *faces, int n, const char *s, int maxw)
{
    for (int k = 0; k < n - 1; k++)
        if (aafont_width(faces[k], s) <= maxw) return faces[k];
    return faces[n - 1];
}

static void pixel(canvas_t *c, int x, int y, uint16_t col)
{
    if (x >= 0 && y >= 0 && x < c->w && y < c->h) c->fb[y * c->w + x] = col;
}

/* A 1 px rule, solid or dotted. Dotted is one pixel in four: in the strip
   two edge lines sit 5 px apart, and at two in four they read as a pair of
   dashed rails competing with the trace; at one in four they are guides. */
static void rule(canvas_t *c, int x0, int x1, int y, uint16_t col, bool dotted)
{
    if (!dotted) { canvas_fill_rect(c, x0, y, x1 - x0 + 1, 1, col); return; }
    for (int x = x0; x <= x1; x++)
        if ((x & 3) == 0) pixel(c, x, y, col);
}

/* A column with no valid reading: hatched, so a gap reads as "no data"
   rather than as clean air or as a line drawn through what was not seen. */
static void hatch(canvas_t *c, int x, int y0, int y1)
{
    for (int y = y0; y <= y1; y++)
        if ((x + y) % 6 == 0) pixel(c, x, y, COL_RULE);
}

static const char *state_name(envs_state_t st)
{
    return st == ENVS_OK ? "GOOD" : st == ENVS_FAIR ? "FAIR" : st == ENVS_POOR ? "POOR" : NULL;
}

/*
 * How far POOR's block reaches past the ink at the sides, for a block `pad`
 * past it above and below: two pixels more. Above and below, the pages have
 * no room to give -- the reading page's block is boxed in between the number
 * and the strip -- and a block as tight at the sides as there looked cramped,
 * its P and R pressed against the edges; the sides are free.
 */
static int block_side(int pad) { return pad > 0 ? pad + 2 : 0; }

/* The width a state word takes in `f`, its block included. */
static int state_width(const aafont_t *f, envs_state_t st, int pad)
{
    const char *w = state_name(st);
    if (w == NULL) return 0;
    return st == ENVS_POOR ? aafont_width(knockout(f), w) + 2 * block_side(pad) : aafont_width(f, w);
}

/*
 * A state's word, what it draws starting at x: GOOD in blue, FAIR in amber,
 * POOR in black on a red block reaching `pad` beyond the ink above and below
 * and block_side(pad) at the sides -- the one state that must be seen from
 * across the room is the one that is a solid patch -- in the knock-out
 * weight. The letters' overshoot (O is a pixel taller than H) stays inside
 * the pad.
 */
static void state_word(canvas_t *c, const aafont_t *f, int x, int top, envs_state_t st, int pad)
{
    const char *w = state_name(st);
    if (w == NULL) return;
    if (st != ENVS_POOR) {
        aafont_draw(c, f, x, top, w, st == ENVS_OK ? COL_GOOD : COL_FAIR, AAFONT_LEFT);
        return;
    }
    canvas_fill_rect(c, x, top - pad, state_width(f, st, pad), f->cap + 2 * pad, COL_POOR);
    aafont_draw(c, knockout(f), x + block_side(pad), top, w, COL_BG, AAFONT_LEFT);
}

/* One arrow, up or down, in an s x s box centred on (cx, cy); its ink is
   0.8 s wide. There is no arrow for steady: the word says it. */
static void arrow(canvas_t *c, float cx, float cy, float s, bool up, uint16_t col)
{
    float sg = up ? -1.0f : 1.0f;
    float xy[] = {
        cx - s * 0.13f, cy - sg * s / 2, cx - s * 0.13f, cy + sg * s * 0.02f,
        cx - s * 0.4f,  cy + sg * s * 0.02f, cx, cy + sg * s / 2,
        cx + s * 0.4f,  cy + sg * s * 0.02f, cx + s * 0.13f, cy + sg * s * 0.02f,
        cx + s * 0.13f, cy - sg * s / 2,
    };
    vec_polygon(c, xy, 7, col);
}

/* The same arrow lying down, pointing right: STEADY's, so the three trends
   are one shape in three directions rather than two arrows and a lone word. */
static void arrow_flat(canvas_t *c, float cx, float cy, float s, uint16_t col)
{
    float xy[] = {
        cx - s / 2,        cy - s * 0.13f, cx + s * 0.02f, cy - s * 0.13f,
        cx + s * 0.02f,    cy - s * 0.4f,  cx + s / 2,     cy,
        cx + s * 0.02f,    cy + s * 0.4f,  cx + s * 0.02f, cy + s * 0.13f,
        cx - s / 2,        cy + s * 0.13f,
    };
    vec_polygon(c, xy, 7, col);
}

/*
 * Numbers are placed by their advance boxes, not their ink. The figures are
 * tabular, so set that way each digit has its own fixed place, and 19.9 going
 * to 20.0 changes the digits and nothing else. Placed by the ink, the whole
 * number moved with its first digit's side bearing -- a leading 4 has 3 px
 * of it at the reading's size, a 1 has 7 -- and jumped sideways by as much
 * as the difference whenever that digit changed. The box's edge goes where
 * the figure that reaches furthest past it lets no ink cross the line: so a
 * leading 1 stands a few pixels in from the margin, as it would in a column.
 */
static int figures_lsb(const aafont_t *f)
{
    int least = 0;
    for (char d = '0'; d <= '9'; d++) {
        char one[2] = { d, '\0' };
        int b = aafont_bearing(f, one);
        if (d == '0' || b < least) least = b;
    }
    return least;
}

static int figures_rsb(const aafont_t *f)
{
    int least = 0;
    for (char d = '0'; d <= '9'; d++) {
        char one[2] = { d, '\0' };
        int b = aafont_advance(f, one) - aafont_bearing(f, one) - aafont_width(f, one);
        if (d == '0' || b < least) least = b;
    }
    return least;
}

/*
 * A reading as the page sets it, in one of the figure faces: the digits, then
 * whatever stands after them. For temperature that is the degree sign, which
 * is in every face and is simply set on the end. For humidity it is a percent
 * sign, because the reading page's "37" is otherwise a bare number whose unit
 * is a small grey "%" in the corner -- from across the room it could as well
 * be a temperature, and the temperature beside it carries its degree sign.
 * The percent is set a size down, about half the digits' height, top to top
 * with them, the way a unit is set after a big figure, PCT_GAP past where the
 * digits' box lets their ink reach -- by the box again, so it holds still.
 */
typedef enum { SUFFIX_NONE, SUFFIX_DEG, SUFFIX_PCT } suffix_t;

#define PCT_GAP 6

typedef struct {
    const aafont_t *f, *pf;  /* the digits' face, and the percent sign's */
    suffix_t sx;
    char s[24];              /* what is set in f: the digits, and a degree sign */
    int lead;                /* the pen starts this far left of the margin */
    int left;                /* the left edge of the ink, from the margin */
    int digits;              /* the right edge of the digits' ink, likewise */
    int pct;                 /* the left edge of the percent sign's ink, likewise */
    int whole;               /* the right edge of everything, likewise */
    int hang;                /* how far below the capitals' top the suffix reaches */
} figure_t;

static void figure(figure_t *g, const aafont_t *f, const aafont_t *pf, const char *b, suffix_t sx)
{
    g->f = f;
    g->pf = pf;
    g->sx = sx;
    snprintf(g->s, sizeof g->s, "%s%s", b, sx == SUFFIX_DEG ? DEG : "");
    /* A minus reaching further left than any figure moves the box rather
       than cross the margin. */
    g->lead = figures_lsb(f);
    int own = aafont_bearing(f, b);
    if (own < g->lead) g->lead = own;
    g->left = own - g->lead;
    g->digits = own + aafont_width(f, b) - g->lead;
    g->pct = aafont_advance(f, b) - figures_rsb(f) + PCT_GAP - g->lead;
    g->whole = sx == SUFFIX_DEG ? aafont_bearing(f, g->s) + aafont_width(f, g->s) - g->lead
             : sx == SUFFIX_PCT ? g->pct + aafont_width(pf, "%")
             : g->digits;
    /* Inter's degree sign is a ring on the capitals' top reaching not quite
       half-way down them; the percent sign is its own face's capitals. */
    g->hang = sx == SUFFIX_DEG ? (f->cap + 1) / 2 : sx == SUFFIX_PCT ? pf->cap : 0;
}

static void figure_draw(canvas_t *c, const figure_t *g, int x, int top, uint16_t col)
{
    aafont_draw(c, g->f, x - g->lead, top, g->s, col, AAFONT_LEFT | AAFONT_ADVANCE);
    if (g->sx == SUFFIX_PCT)
        aafont_draw(c, g->pf, x + g->pct, top, "%", col, AAFONT_LEFT);
}

/* How long the gas sensor has been at it. Never a time remaining: the chip
   cannot know how long its own warm-up will take. */
static void so_far(char *out, size_t size, int minutes)
{
    if (minutes < 0) minutes = 0;
    if (minutes < 1000) snprintf(out, size, "%d MIN SO FAR", minutes);
    else if (minutes / 60 < 10000) snprintf(out, size, "%d H SO FAR", minutes / 60);
    else snprintf(out, size, "9999+ H SO FAR");
}

/* Which page of how many, as dots centred under the page; the current one
   filled. Pages beyond a dozen would not fit, and are not drawn. */
static void page_dots(canvas_t *c, int page, int pages)
{
    if (pages < 2 || pages > 12) return;
    float x = 160.0f - (float)(pages - 1) * 5.0f;
    for (int k = 0; k < pages; k++, x += 10.0f) {
        if (k == page) vec_disc(c, x, R_DOTS_Y, 2.5f, COL_WHITE);
        else vec_ring(c, x, R_DOTS_Y, 2.2f, 1.0f, COL_GREY);
    }
}

/* ---- the plot, strip and chart alike --------------------------------------- */

typedef struct {
    int x0, x1;              /* columns, inclusive */
    int floor;               /* the floor row; the plot is the h rows above */
    float h;
    float line_w, dot_r;
    bool full;               /* the full chart: solid lines, key, peak, axis */
} plot_t;

/* Column means, worked out once per plot. Static rather than on the stack or
   the heap: drawing allocates nothing, and one plot is drawn at a time. */
#define MAX_COLS 320
static int32_t s_mean[MAX_COLS];
static bool s_have[MAX_COLS];

/*
 * Each column's mean of its valid 5-minute slots, and the first column that
 * has any. A column spans ENVUI_SLOTS / width slots: about 1.2 on the chart,
 * where two slots share some columns, and exactly one or none on the strip,
 * which is wider than the day has slots -- there a column may repeat its
 * neighbour's slot, and must not run past the last one.
 */
static int columns(const envui_series_t *s, int pw)
{
    int first = -1;
    if (pw > MAX_COLS) pw = MAX_COLS;
    for (int px = 0; px < pw; px++) {
        int i0 = (int)((long)px * N / pw), i1 = (int)((long)(px + 1) * N / pw);
        if (i1 <= i0) i1 = i0 + 1;
        if (i1 > N) i1 = N;
        int64_t sum = 0;
        int n = 0;
        for (int i = i0; i < i1; i++)
            if (s->valid[i]) { sum += s->slot[i]; n++; }
        s_have[px] = n > 0;
        s_mean[px] = n > 0 ? (int32_t)(sum / n) : 0;
        if (n > 0 && first < 0) first = px;
    }
    return first;
}

static int yline(const plot_t *p, envs_series_t s, int32_t v)
{
    return (int)lroundf((float)p->floor - level(s, v, p->h));
}

/* Where midnight falls, in slots. Exact when the caller knows it; otherwise
   from the latest slot's hour, as if that slot began on the hour (out by
   under an hour, and only ever used for ticks). INT32_MIN when neither. */
static int midnight(const envui_series_t *s, bool *exact)
{
    *exact = s->midnight_slot >= 0 && s->midnight_slot < N;
    if (*exact) return s->midnight_slot;
    if (s->last_slot_hour >= 0 && s->last_slot_hour < 24) return N - 1 - s->last_slot_hour * 12;
    return INT32_MIN;
}

static int slot_x(const plot_t *p, int slot)
{
    return p->x0 + (int)((long)slot * (p->x1 - p->x0 + 1) / N);
}

/* The midnight line: a 1 px grey rule through the plot, so the two days on
   the chart are told apart without reading the axis. */
static void midnight_line(canvas_t *c, const envui_series_t *s, const plot_t *p)
{
    bool exact;
    int mid = midnight(s, &exact);
    if (!exact) return;
    int x = slot_x(p, mid);
    canvas_fill_rect(c, x, p->floor - (int)p->h, 1, (int)p->h, COL_GREY);
}

/*
 * Hours 18, 06 and 12 on 3 px ticks, the weekday at midnight in white, and
 * NOW at the right. A label that would touch NOW, or cross the left margin,
 * is dropped rather than squeezed.
 */
static void time_axis(canvas_t *c, const envui_series_t *s, const plot_t *p)
{
    int now_x = p->x1 + 1 - aafont_width(F_LABEL, "NOW");
    aafont_draw(c, F_LABEL, now_x, D_AXIS, "NOW", COL_WHITE, AAFONT_LEFT);

    bool exact;
    int mid = midnight(s, &exact);
    if (mid == INT32_MIN) return;
    char day[4] = "00";
    if (exact && s->midnight_day != NULL && s->midnight_day[0] != '\0') {
        int n = 0;
        while (n < 3 && s->midnight_day[n] != '\0') { day[n] = s->midnight_day[n]; n++; }
        day[n] = '\0';
    }
    for (int k = -4; k <= 4; k++) {
        int slot = mid + k * 72;              /* six hours apart */
        if (slot < 0 || slot >= N) continue;
        int x = slot_x(p, slot);
        int hour = ((k * 6) % 24 + 24) % 24;
        bool is_mid = hour == 0;
        if (!(is_mid && exact)) canvas_fill_rect(c, x, p->floor + 1, 1, 3, COL_GREY);
        const char *lab = is_mid ? day : hour == 6 ? "06" : hour == 12 ? "12" : "18";
        /* Centred on the tick's pixel column, its middle at x + 0.5. */
        int w = aafont_width(F_LABEL, lab), lx = x - (w - 1) / 2;
        if (lx < XL || lx + w > now_x - 4) continue;
        aafont_draw(c, F_LABEL, lx, D_AXIS, lab, is_mid && exact ? COL_WHITE : COL_GREY, AAFONT_LEFT);
    }
}

static zones_t zones(envs_series_t s, const plot_t *p)
{
    zones_t z = { 0.0f, 0.0f, false };
    const envs_limits_t *L = is_gas(s) ? envs_limits(s) : NULL;
    if (L == NULL) return z;
    z.fair = (float)p->floor - level(s, L->fair, p->h);
    z.poor = (float)p->floor - level(s, L->poor, p->h);
    z.zoned = true;
    return z;
}

/*
 * One joint of the trace, split where it crosses a zone line and each piece
 * drawn in the colour of the zone it is in. Coloured by either end alone, a
 * line coming down out of a POOR hour was white all the way through FAIR and
 * POOR, or a rise out of GOOD amber from the floor up -- ink saying a state
 * the air was not in, on the very lines that name the states.
 */
static void trace_seg(canvas_t *c, const zones_t *z, float w,
                      float x0, float y0, float x1, float y1)
{
    float t[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    int n = 1;
    if (z->zoned) {
        const float line[2] = { z->fair, z->poor };
        for (int k = 0; k < 2; k++)
            if ((y0 - line[k]) * (y1 - line[k]) < 0.0f) t[n++] = (line[k] - y0) / (y1 - y0);
        if (n == 3 && t[2] < t[1]) { float u = t[1]; t[1] = t[2]; t[2] = u; }
    }
    t[n] = 1.0f;
    for (int k = 0; k < n; k++) {
        float ya = y0 + (y1 - y0) * t[k], yb = y0 + (y1 - y0) * t[k + 1];
        vec_line(c, x0 + (x1 - x0) * t[k], ya, x0 + (x1 - x0) * t[k + 1], yb, w,
                 zone_colour(z, (ya + yb) / 2.0f));
    }
}

/*
 * The trace: each column's mean joined by a line, coloured by the zone it
 * runs through. Nothing is drawn across a gap; a reading with gaps both sides
 * is a dot twice the line's width, not nothing and not a speck. It ends at
 * the now-dot -- the display value, not the last column -- joined to it only
 * if the last column was measured.
 */
static void trace(canvas_t *c, const envui_series_t *s, const plot_t *p)
{
    zones_t z = zones(s->series, p);
    int pw = p->x1 - p->x0 + 1;
    float lx = 0.0f, ly = 0.0f;
    int run = 0;
    for (int px = 0; px < pw && px < MAX_COLS; px++) {
        if (!s_have[px]) {
            if (run == 1) vec_disc(c, lx, ly, p->line_w, zone_colour(&z, ly));
            run = 0;
            continue;
        }
        float x = (float)(p->x0 + px) + 0.5f;
        float y = (float)p->floor - level(s->series, s_mean[px], p->h);
        if (run > 0) trace_seg(c, &z, p->line_w, lx, ly, x, y);
        lx = x; ly = y;
        run++;
    }
    if (!s->have_now || gas_waiting(s)) {
        if (run == 1) vec_disc(c, lx, ly, p->line_w, zone_colour(&z, ly));
        return;
    }
    /* A pinned reading's dot stays inside the plot: at the top edge itself
       it would reach up into the header and sit on the number or the POOR
       block over it. */
    float nx = (float)p->x1 + 0.5f;
    float ny = (float)p->floor - level(s->series, s->now_value, p->h);
    float ny_top = (float)p->floor - p->h + p->dot_r;
    if (ny < ny_top) ny = ny_top;
    if (run > 0) trace_seg(c, &z, p->line_w, lx, ly, nx, ny);
    vec_disc(c, nx, ny, p->dot_r, COL_WHITE);
}

/*
 * In the strip, a bad stretch filled from the FAIR line up to the trace, a
 * column at a time, each row in the colour of the zone it is in. At 14 px
 * the FAIR band is 4 px and a line through it is a 1-2 px amber tick nobody
 * sees from a chair; filled, a bad hour is a block of colour, as it is in
 * the week grid. GOOD is left as a line: clean air is not news.
 */
static void strip_fill(canvas_t *c, const envui_series_t *s, const plot_t *p)
{
    const envs_limits_t *L = is_gas(s->series) ? envs_limits(s->series) : NULL;
    if (L == NULL) return;
    int yf = yline(p, s->series, L->fair), yp = yline(p, s->series, L->poor);
    int pw = p->x1 - p->x0 + 1;
    for (int px = 0; px < pw && px < MAX_COLS; px++) {
        if (!s_have[px] || s_mean[px] < L->fair) continue;
        int yt = (int)floorf((float)p->floor - level(s->series, s_mean[px], p->h));
        for (int y = yt; y <= yf; y++)
            pixel(c, p->x0 + px, y, y <= yp ? COL_POOR : COL_FAIR);
    }
}

/*
 * The day's peak, as the flash log stored it (the highest 5-minute maximum,
 * above the means the trace shows): a triangle over it and the number beside
 * it on a black knock-out. Only when the day reached FAIR -- a GOOD day's
 * peak is not news -- and only when it is not next to now, where the number
 * at the top of the page already says it.
 */
static void peak(canvas_t *c, const envui_series_t *s, const plot_t *p)
{
    const envs_limits_t *L = is_gas(s->series) ? envs_limits(s->series) : NULL;
    if (L == NULL || s->peak_slot < 0 || s->peak_slot >= N || s->peak_value < L->fair) return;
    int col = slot_x(p, s->peak_slot) - p->x0;
    float px = (float)(p->x0 + col) + 0.5f;
    if ((float)p->x1 + 0.5f - px <= 20.0f) return;
    int top = p->floor - (int)p->h;
    float py = (float)p->floor - level(s->series, s->peak_value, p->h);
    if (py < (float)top + 6.0f) py = (float)top + 6.0f;     /* pinned peaks keep their marker in the plot */
    float tip = py - 3.0f;

    /*
     * The stored maximum is a 30 s reading and the trace is 5-minute means,
     * so a short spike's marker can stand well clear of the line it belongs
     * to, over empty chart. A 1 px grey stem from the top of the trace near
     * it up to the tip ties the two together. It passes behind the zone
     * lines, which stay whole: they are the scale, the stem only a pointer.
     */
    float trace_top = -1.0f;
    int pw = p->x1 - p->x0 + 1;
    for (int k = col - 1; k <= col + 1; k++) {
        if (k < 0 || k >= pw || k >= MAX_COLS || !s_have[k]) continue;
        float y = (float)p->floor - level(s->series, s_mean[k], p->h);
        if (trace_top < 0.0f || y < trace_top) trace_top = y;
    }
    if (trace_top >= 0.0f && trace_top - tip > 4.0f) {
        int x = (int)px;
        for (int y = (int)tip; y < (int)trace_top; y++) {
            if (x < 0 || y < 0 || x >= c->w || y >= c->h) continue;
            uint16_t under = c->fb[y * c->w + x];
            if (under != COL_FAIR && under != COL_POOR) c->fb[y * c->w + x] = COL_GREY;
        }
    }

    float tri[] = { px - 4, py - 9, px + 4, py - 9, px, tip };
    vec_polygon(c, tri, 3, COL_WHITE);

    /*
     * The number beside the triangle on a black knock-out, on whichever side
     * keeps it 12 px clear of the plot's end, where the now-dot and the key
     * would otherwise run into it ("1100 POOR"). The knock-out reaches one
     * row and two columns past the ink; if a zone line would run through it,
     * the number drops to just under that line rather than cutting it -- a
     * zone line with a hole in it is an edge the eye cannot follow across.
     */
    char b[16];
    format_value(s->series, s->peak_value, b, sizeof b);
    int w = aafont_width(F_LABEL, b), cap = F_LABEL->cap;
    int lx = (int)px + 7;
    if (lx + w > p->x1 - 12) lx = (int)px - 7 - w;
    int ly = (int)py - 3 - cap;
    if (ly < top) ly = top;
    int line[2] = { yline(p, s->series, L->poor), yline(p, s->series, L->fair) };
    for (int k = 0; k < 2; k++)
        if (line[k] >= ly - 1 && line[k] <= ly + cap) ly = line[k] + 3;
    canvas_fill_rect(c, lx - 2, ly - 1, w + 4, cap + 2, COL_BG);
    aafont_draw(c, F_LABEL, lx, ly, b, COL_WHITE, AAFONT_LEFT);
}

/* The key beside the full chart: each zone's word in its colour, and the
   two edges' values in grey, centred on their lines. */
static void key(canvas_t *c, const envui_series_t *s, const plot_t *p)
{
    char b[16];
    const aafont_t *f = F_LABEL;
    int half = f->cap / 2;
    const envs_limits_t *L = is_gas(s->series) ? envs_limits(s->series) : NULL;
    if (L != NULL) {
        int yf = yline(p, s->series, L->fair), yp = yline(p, s->series, L->poor);
        aafont_draw(c, f, D_KEY_X, p->floor - (int)p->h + 1, "POOR", COL_POOR, AAFONT_LEFT);
        snprintf(b, sizeof b, "%ld", (long)L->poor);
        aafont_draw(c, f, D_KEY_X, yp - half, b, COL_GREY, AAFONT_LEFT);
        aafont_draw(c, f, D_KEY_X, (yf + yp) / 2 - half, "FAIR", COL_FAIR, AAFONT_LEFT);
        snprintf(b, sizeof b, "%ld", (long)L->fair);
        aafont_draw(c, f, D_KEY_X, yf - half, b, COL_GREY, AAFONT_LEFT);
        /* eCO2 is never called GOOD: it is estimated from the VOCs, so a low
           one says nothing the VOC page has not. */
        if (s->series == ENVS_VOC)
            aafont_draw(c, f, D_KEY_X, yf + 14, "GOOD", COL_GOOD, AAFONT_LEFT);
        return;
    }
    const meta_t *m = &s_meta[s->series];
    for (int k = 0; k < 2; k++) {
        snprintf(b, sizeof b, "%ld", (long)(m->ref[k] / 100));
        aafont_draw(c, f, D_KEY_X, yline(p, s->series, m->ref[k]) - half, b, COL_GREY, AAFONT_LEFT);
    }
}

static void plot(canvas_t *c, const envui_series_t *s, const plot_t *p)
{
    const envs_limits_t *L = is_gas(s->series) ? envs_limits(s->series) : NULL;
    const meta_t *m = &s_meta[s->series];
    int pw = p->x1 - p->x0 + 1, top = p->floor - (int)p->h;
    int first = columns(s, pw);
    /* The two lines a plot draws: the zone edges, or the reference lines. */
    int ya = yline(p, s->series, L != NULL ? L->fair : m->ref[0]);
    int yb = yline(p, s->series, L != NULL ? L->poor : m->ref[1]);

    /* A faint blue under GOOD, on the VOC chart only: blue means GOOD, and
       eCO2 is never GOOD. Not in the strip, where six rows of it read as
       data -- on a strip with no data, as the only thing there -- and not
       on a chart with nothing in it to be GOOD. */
    if (p->full && first >= 0 && s->series == ENVS_VOC && L != NULL)
        canvas_fill_rect(c, p->x0, ya, pw, p->floor - ya, COL_TINT);
    /* Hatched from the first reading on: a board just switched on shows an
       empty plot, not a day of apparent failure. */
    for (int px = first < 0 ? pw : first; px < pw && px < MAX_COLS; px++)
        if (!s_have[px]) hatch(c, p->x0 + px, top, p->floor - 1);
    /* Midnight before the lines, so the zone lines it crosses stay whole;
       and only over a day that has something in it to divide. */
    if (p->full && first >= 0) midnight_line(c, s, p);
    if (!p->full) strip_fill(c, s, p);

    /* The lines that name the zones: solid on the chart, dotted in the
       strip, where they would otherwise outweigh a trace that is barely
       14 px from floor to top. Temperature's and humidity's reference lines
       only on the chart, where the key labels them; in the strip they were
       unlabelled grey dots lying on the trace, saying nothing. */
    if (L != NULL) {
        rule(c, p->x0, p->x1, ya, COL_FAIR, !p->full);
        rule(c, p->x0, p->x1, yb, COL_POOR, !p->full);
    } else if (p->full) {
        rule(c, p->x0, p->x1, ya, COL_RULE, false);
        rule(c, p->x0, p->x1, yb, COL_RULE, false);
    }
    rule(c, p->x0, p->x1, p->floor, COL_RULE, false);

    trace(c, s, p);

    if (!p->full) return;
    key(c, s, p);
    peak(c, s, p);
    time_axis(c, s, p);
    if (first < 0) {
        /* Between the two lines, where nothing crosses it, so it needs no
           knock-out to be read. */
        aafont_draw(c, F_LABEL, (p->x0 + p->x1 + 1) / 2, (ya + yb) / 2 - F_LABEL->cap / 2,
                    "NO DATA", COL_GREY, AAFONT_CENTRE);
    }
}

/* ---- the reading page ---------------------------------------------------- */

/*
 * The number's sizes, largest first, and each one's percent sign. It is as
 * big as fits -- 200 px at most, and clear of the trend word -- and steps
 * down a size at a time, as the stroke number shrank 2 px at a time.
 */
static const aafont_t *const s_num_face[] = { F_READING, F_NUMBER, F_STATE };
static const aafont_t *const s_pct_face[] = { F_WORD, F_WORD, F_SUB };
#define NUM_FACES ((int)(sizeof s_num_face / sizeof s_num_face[0]))

/* WARMING UP and GAS ERROR, where the number would be: the state words' size
   when it fits, the next down when not (WARMING UP is 291 px in it). */
static const aafont_t *const s_wait_face[] = { F_STATE, F_WORD };

/*
 * The trend, right-aligned: an arrow and RISING or FALLING in white, or
 * STEADY in grey alone. Nothing until there is enough history to say. The
 * arrow is what carries across the room, and keeps its size, at the top of
 * the number's band; the word under it is the secondary size, standing on
 * the number's baseline -- set as large as the stroke font set it, Inter's
 * wider letters left the number beside it no room. Returns the left edge of
 * what stands level with the number's foot, for the number to keep clear of.
 */
static int reading_trend(canvas_t *c, envs_trend_t tr)
{
    const char *w = tr == ENVS_RISING ? "RISING" : tr == ENVS_FALLING ? "FALLING"
                  : tr == ENVS_STEADY ? "STEADY" : NULL;
    if (w == NULL) return XR + R_NUM_GAP;
    bool moving = tr != ENVS_STEADY;
    float top = (float)(R_NUM_BASE - F_READING->cap);
    if (moving)
        arrow(c, XR - 0.4f * R_ARROW, top + R_ARROW / 2, R_ARROW, tr == ENVS_RISING, COL_WHITE);
    else
        arrow_flat(c, XR - 0.5f * R_ARROW, top + R_ARROW / 2, R_ARROW, COL_GREY);
    aafont_draw(c, F_SUB, XR, R_NUM_BASE - F_SUB->cap, w, moving ? COL_WHITE : COL_GREY, AAFONT_RIGHT);
    return XR - aafont_width(F_SUB, w);
}

/*
 * The reading, as big as fits. Two things limit it: 200 px of ink in all, and
 * the trend word, which it must stay 10 px clear of. The trend word is low, on
 * the number's baseline, and a degree or percent sign hangs from the top of
 * the digits; so where the suffix ends well above the word's capitals, only
 * the digits need to clear it, and the suffix may stand over the word's
 * shoulder -- which is what lets "22.5°" keep its size beside STEADY. The
 * arrow over the word is always clear: 16 + 200 + 10 is well left of it.
 */
static void reading_number(canvas_t *c, const envui_series_t *s, int trend_left)
{
    char b[16];
    format_value(s->series, s->now_value, b, sizeof b);
    suffix_t sx = s->series == ENVS_TEMP ? SUFFIX_DEG : s->series == ENVS_RH ? SUFFIX_PCT : SUFFIX_NONE;
    int word_top = R_NUM_BASE - F_SUB->cap;
    figure_t g;
    for (int k = 0; k < NUM_FACES; k++) {
        figure(&g, s_num_face[k], s_pct_face[k], b, sx);
        int top = R_NUM_BASE - g.f->cap;
        int low = top + g.hang + 3 <= word_top ? g.digits : g.whole;
        if (g.whole - g.left <= R_NUM_MAXW && XL + low + R_NUM_GAP <= trend_left
            && XL + g.whole + R_SUFFIX_GAP <= trend_left) break;
    }
    figure_draw(c, &g, XL, R_NUM_BASE - g.f->cap, COL_WHITE);
}

static void reading_page(canvas_t *c, const envui_series_t *s, int page, int pages)
{
    canvas_clear(c);
    if (s == NULL || !series_ok(s->series)) return;
    const meta_t *m = &s_meta[s->series];

    aafont_draw(c, F_LABEL, XL, R_LABEL_TOP, m->label, COL_GREY, AAFONT_LEFT);
    aafont_draw(c, F_LABEL, XR, R_LABEL_TOP, m->unit, COL_GREY, AAFONT_RIGHT);

    int band = R_NUM_BASE - F_READING->cap;     /* the top of the number's band */
    if (gas_waiting(s)) {
        /* No number while the chip settles: only how long it has been at it.
           Not under an error: the minutes count from the sensor's start, and
           beneath GAS ERROR a count going up reads as progress that is not
           being made. */
        const char *t = wait_text(s->gas_error);
        aafont_draw(c, fit(s_wait_face, 2, t, XR - XL), XL, band, t, COL_GREY, AAFONT_LEFT);
        if (!s->gas_error) {
            char b[24];
            so_far(b, sizeof b, s->warm_minutes);
            aafont_draw(c, F_SUB, XL, R_NUM_BASE - F_SUB->cap, b, COL_GREY, AAFONT_LEFT);
        }
    } else if (!s->have_now) {
        /* A sensor that is not answering: say so, rather than leave a "--"
           that could as well mean "wait". */
        aafont_draw(c, F_READING, XL, band, "--", COL_GREY, AAFONT_LEFT);
        aafont_draw(c, F_WORD, XL, R_WORD_BASE - F_WORD->cap, "NO READING", COL_GREY, AAFONT_LEFT);
    } else {
        reading_number(c, s, reading_trend(c, s->trend));

        /* POOR's block starts its pad left of the margin, so its letters line
           up with the number's at x 16 rather than sitting indented under
           it: the block is not text, and nothing there is near a corner. */
        if (s->series == ENVS_ECO2 && s->state == ENVS_OK)
            /* eCO2 is never GOOD: below its first limit it says where it comes from. */
            aafont_draw(c, F_WORD, XL, R_WORD_BASE - F_WORD->cap, "FROM VOCs", COL_GREY, AAFONT_LEFT);
        else if (is_gas(s->series))
            state_word(c, F_STATE, s->state == ENVS_POOR ? XL - block_side(R_WORD_PAD) : XL, R_WORD_TOP,
                       s->state, R_WORD_PAD);
    }

    plot_t p = { XL, XR, R_STRIP_FLOOR, R_STRIP_H, 1.6f, 2.5f, false };
    plot(c, s, &p);
    aafont_draw(c, F_LABEL, XL, R_STRIP_LABEL, MINUS "24H", COL_GREY, AAFONT_LEFT);
    aafont_draw(c, F_LABEL, XR, R_STRIP_LABEL, "NOW", COL_GREY, AAFONT_RIGHT);
    page_dots(c, page, pages);
}

/* ---- the full chart ------------------------------------------------------ */

/* The header's number, the state words' size stepping down to the next;
   and the line that says why there is none, from the chart word's size down
   to the labels' (NO READING beside HUMIDITY 24H has 153 px). */
static const aafont_t *const s_head_face[] = { F_STATE, F_WORD };
static const aafont_t *const s_head_msg_face[] = { F_WORD, F_SUB, F_LABEL };

/*
 * The header's number, right-aligned by its advance box (see figures_lsb) so
 * that no figure's ink passes `r`: its ink's left edge.
 */
static int head_left(const aafont_t *f, const char *t, int r)
{
    return r + figures_rsb(f) - aafont_advance(f, t) + aafont_bearing(f, t);
}

/*
 * The header's right side: the word at the right edge, then the arrow, then
 * the number, which steps down a size to keep 12 px clear of the title. If
 * it still cannot, the arrow goes -- the reading page behind this one shows
 * the trend in words anyway.
 */
static void detail_header(canvas_t *c, const envui_series_t *s, int title_r)
{
    /* No number to give: why not, in words, in grey. NO READING rather than a
       "--", which at this size is a 4 px smudge that says nothing. */
    if (gas_waiting(s) || !s->have_now) {
        const char *msg = gas_waiting(s) ? wait_text(s->gas_error) : "NO READING";
        const aafont_t *f = fit(s_head_msg_face, 3, msg, XR - title_r);
        aafont_draw(c, f, XR, D_BASE - f->cap, msg, COL_GREY, AAFONT_RIGHT);
        return;
    }

    int right = XR;
    bool word = is_gas(s->series) && state_name(s->state) != NULL
             && !(s->series == ENVS_ECO2 && s->state == ENVS_OK);
    if (word) {
        int pad = s->state == ENVS_POOR ? D_WORD_PAD : 0;
        int x = XR - state_width(F_WORD, s->state, pad);
        state_word(c, F_WORD, x, D_BASE - F_WORD->cap, s->state, pad);
        right = x - 10;
    }

    /* The degree sign stays, being part of how a temperature is written; the
       percent sign does not, the unit under the title already saying "%". */
    char b[16], t[24];
    format_value(s->series, s->now_value, b, sizeof b);
    snprintf(t, sizeof t, "%s%s", b, s->series == ENVS_TEMP ? DEG : "");
    bool moving = s->trend == ENVS_RISING || s->trend == ENVS_FALLING;
    for (int pass = moving ? 0 : 1; pass < 2; pass++) {
        int r = pass == 0 ? right - (int)(D_ARROW * 0.8f) - 10 : right;
        const aafont_t *f = s_head_face[0];
        if (head_left(f, t, r) < title_r) f = s_head_face[1];
        if (pass == 0 && head_left(f, t, r) < title_r) continue;
        if (pass == 0)
            arrow(c, (float)right - D_ARROW * 0.4f, D_ARROW_CY, D_ARROW, s->trend == ENVS_RISING, COL_WHITE);
        aafont_draw(c, f, r + figures_rsb(f), D_BASE - f->cap, t, COL_WHITE, AAFONT_RIGHT | AAFONT_ADVANCE);
        break;
    }
}

static void detail_page(canvas_t *c, const envui_series_t *s)
{
    canvas_clear(c);
    if (s == NULL || !series_ok(s->series)) return;
    const meta_t *m = &s_meta[s->series];

    /* The header's right side keeps 12 px clear of the longer of the two
       title lines: its number is tall enough to span both. */
    int tw = aafont_draw(c, F_LABEL, XL, D_TITLE_TOP, m->title, COL_WHITE, AAFONT_LEFT);
    int sw = aafont_draw(c, F_LABEL, XL, D_BASE - F_LABEL->cap, m->subtitle, COL_GREY, AAFONT_LEFT);
    detail_header(c, s, XL + (tw > sw ? tw : sw) + 12);

    plot_t p = { D_X0, D_X1, D_FLOOR, D_H, 2.0f, 3.5f, true };
    plot(c, s, &p);
}

/* ---- the week ------------------------------------------------------------ */

/*
 * Seven days by 24 hours, each hour the worst state that lasted in it (the
 * caller decides what lasted). Ink is for exceptions: an OK hour is a thin
 * blue bar, FAIR half a cell of amber, POOR the whole cell in red -- so the
 * height of a mark says how bad before its colour does.
 */
static void week_page(canvas_t *c, const envui_week_t *w, int page, int pages)
{
    canvas_clear(c);
    if (w == NULL) return;

    aafont_draw(c, F_LABEL, XL, 8, "WEEK", COL_WHITE, AAFONT_LEFT);

    bool any = false;
    for (int d = 0; d < 7; d++) {
        /*
         * The day in upper and lower case, "Mo" for "MO": in Inter a pair of
         * capitals is up to 28 px, and the column before the grid has 28; in
         * lower case the widest, "We", is 26 and leaves the grid a gap.
         */
        char name[3] = { w->day[d][0], w->day[d][1], '\0' };
        if (name[1] >= 'A' && name[1] <= 'Z') name[1] = (char)(name[1] - 'A' + 'a');
        aafont_draw(c, F_LABEL, XL, W_GY + d * W_CH, name, d == 6 ? COL_WHITE : COL_GREY, AAFONT_LEFT);
        for (int h = 0; h < 24; h++) {
            int x = W_GX + h * W_CW, y = W_GY + d * W_CH + 1;
            switch (w->cell[d][h]) {
            case ENVUI_CELL_NONE:
                canvas_fill_rect(c, x + W_CELL_W / 2 - 1, y + W_CELL_H / 2 - 1, 2, 2, COL_RULE);
                break;
            case ENVUI_CELL_OK:
                canvas_fill_rect(c, x, y + W_CELL_H - 3, W_CELL_W, 3, COL_GOOD);
                any = true;
                break;
            case ENVUI_CELL_FAIR:
                canvas_fill_rect(c, x, y + W_CELL_H - 7, W_CELL_W, 7, COL_FAIR);
                any = true;
                break;
            case ENVUI_CELL_POOR:
                canvas_fill_rect(c, x, y, W_CELL_W, W_CELL_H, COL_POOR);
                any = true;
                break;
            default:                         /* still to come: nothing */
                break;
            }
        }
    }

    /* The takeaway: how many hours went bad, or that none did. */
    char b[24];
    int poor = w->poor_hours < 0 ? 0 : w->poor_hours > 7 * 24 ? 7 * 24 : w->poor_hours;
    int fair = w->fair_hours < 0 ? 0 : w->fair_hours > 7 * 24 ? 7 * 24 : w->fair_hours;
    if (poor == 0 && fair == 0) {
        aafont_draw(c, F_LABEL, XR, 8, any ? "ALL GOOD" : "NO DATA", any ? COL_GOOD : COL_GREY,
                    AAFONT_RIGHT);
    } else {
        int x = XR;
        if (fair > 0) {
            snprintf(b, sizeof b, "FAIR %dH", fair);
            x -= aafont_draw(c, F_LABEL, x, 8, b, COL_FAIR, AAFONT_RIGHT);
            x -= 16;
        }
        if (poor > 0) {
            snprintf(b, sizeof b, "POOR %dH", poor);
            aafont_draw(c, F_LABEL, x, 8, b, COL_POOR, AAFONT_RIGHT);
        }
    }

    /* 00 at the grid's left, 06/12/18 centred on the gaps before those
       hours, 24 at its right -- ending on the margin, a pixel inside the
       grid's last column. Their ink is 141..153: 3 px under the last row's
       cells and 6 px over the page dots, so "12" reads as a label on the
       grid and not as a caption on the dots under it. */
    int ly = W_GY + 7 * W_CH + 1;
    aafont_draw(c, F_LABEL, W_GX, ly, "00", COL_GREY, AAFONT_LEFT);
    const char *mid[] = { "06", "12", "18" };
    for (int k = 0; k < 3; k++) {
        int x = W_GX + (k + 1) * 6 * W_CW - 1;
        aafont_draw(c, F_LABEL, x, ly, mid[k], COL_GREY, AAFONT_CENTRE);
    }
    aafont_draw(c, F_LABEL, XR + 1, ly, "24", COL_GREY, AAFONT_RIGHT);

    page_dots(c, page, pages);
}

/* ---- the pages, in linear light ----------------------------------------------- */

/*
 * The marks -- the trace, the arrows, the peak's triangle, the dots -- blend
 * their edges in linear light, as the type does, rather than vector.c's
 * default of mixing the codes: that drew an arrow's stem with edges of 139
 * and 35 where the type beside it would have 194 and 104, so every mark had a
 * darker, thinner rim than the letters next to it. vector.c is the watch
 * face's too, and keeps its default for it; envui turns linear light on for
 * a page and puts back what it found.
 */
void envui_reading(canvas_t *c, const envui_series_t *s, int page, int pages)
{
    bool was = vec_linear_light(true);
    reading_page(c, s, page, pages);
    vec_linear_light(was);
}

void envui_detail(canvas_t *c, const envui_series_t *s)
{
    bool was = vec_linear_light(true);
    detail_page(c, s);
    vec_linear_light(was);
}

void envui_week(canvas_t *c, const envui_week_t *w, int page, int pages)
{
    bool was = vec_linear_light(true);
    week_page(c, w, page, pages);
    vec_linear_light(was);
}

/* ---- the verdict ----------------------------------------------------------- */

/*
 * One line for the clock page: which reading is at fault and how bad, or
 * AIR GOOD when neither is. The name is white and the word carries the
 * state, so the colour is never the only thing saying it.
 */
static bool verdict_has_word(envs_verdict_t v)
{
    return v.state == ENVS_OK || v.state == ENVS_FAIR || v.state == ENVS_POOR;
}

static const char *verdict_who(envs_verdict_t v)
{
    return v.state == ENVS_OK ? "AIR" : v.worst == ENVS_ECO2 ? "eCO2" : "VOC";
}

/* A size worth drawing, and a place near enough the panel to be worth
   drawing at; anything else draws nothing rather than text a million pixels
   off, or a float turned to an int it does not fit. */
static bool verdict_size(float *size)
{
    if (!isfinite(*size) || *size <= 0.0f) return false;
    if (*size > 200.0f) *size = 200.0f;
    return true;
}

static bool verdict_place(float x, float y)
{
    return isfinite(x) && isfinite(y)
        && x >= -5000.0f && x <= 5000.0f && y >= -5000.0f && y <= 5000.0f;
}

/* The face whose capitals are nearest `size`, of those that have letters;
   a tie goes to the smaller. */
static const aafont_t *verdict_face(float size)
{
    static const aafont_t *const faces[] = { F_LABEL, F_SUB, F_WORD, F_STATE };
    const aafont_t *best = faces[0];
    for (int k = 1; k < 4; k++)
        if (fabsf((float)faces[k]->cap - size) < fabsf((float)best->cap - size)) best = faces[k];
    return best;
}

/* The POOR block's reach past the ink above and below, an eighth of the
   capitals: 4 px round the state words, as on the reading page. */
static int verdict_pad(const aafont_t *f) { return (f->cap + 4) / 8; }

/* Between the name and the word, or the word's block: a word space. */
static int verdict_gap(const aafont_t *f) { return aafont_advance(f, " "); }

static int nearest(float v) { return (int)floorf(v + 0.5f); }

void envui_verdict(canvas_t *c, float x, float y, float size, envs_verdict_t v)
{
    if (!verdict_has_word(v)) {
        envui_verdict_wait(c, x, y, size, false);
        return;
    }
    if (!verdict_size(&size) || !verdict_place(x, y)) return;
    const aafont_t *f = verdict_face(size);
    int ix = nearest(x), iy = nearest(y);
    int ww = aafont_draw(c, f, ix, iy, verdict_who(v), COL_WHITE, AAFONT_LEFT);
    int pad = v.state == ENVS_POOR ? verdict_pad(f) : 0;
    state_word(c, f, ix + ww + verdict_gap(f), iy, v.state, pad);
}

/* The same line's width, worked out the way envui_verdict lays it out, so the
   clock can centre it without knowing the type's metrics. */
float envui_verdict_width(float size, envs_verdict_t v)
{
    if (!verdict_has_word(v)) return envui_verdict_wait_width(size, false);
    if (!verdict_size(&size)) return 0.0f;
    const aafont_t *f = verdict_face(size);
    int pad = v.state == ENVS_POOR ? verdict_pad(f) : 0;
    return (float)(aafont_width(f, verdict_who(v)) + verdict_gap(f) + state_width(f, v.state, pad));
}

/*
 * No verdict yet, and why, in grey: the chip warming up, or the chip saying
 * its data is invalid. The two must not share a word -- a clock that said
 * WARMING UP while the VOC page one swipe away said GAS ERROR would be
 * contradicting it, and warming up promises a verdict an error never brings.
 */
void envui_verdict_wait(canvas_t *c, float x, float y, float size, bool gas_error)
{
    if (!verdict_size(&size) || !verdict_place(x, y)) return;
    aafont_draw(c, verdict_face(size), nearest(x), nearest(y), wait_text(gas_error), COL_GREY, AAFONT_LEFT);
}

float envui_verdict_wait_width(float size, bool gas_error)
{
    if (!verdict_size(&size)) return 0.0f;
    return (float)aafont_width(verdict_face(size), wait_text(gas_error));
}

/* ---- the clock ------------------------------------------------------------- */

/*
 * envo's clock page. The time is set in the figure face the clock was cut
 * for and centred by its advance, not its ink, so the digits hold still as
 * they change: tabular figures give "11:11:11" and "20:08:00" one advance,
 * though a 1 has less ink than a 0. The date is the secondary size over it.
 * The verdict is the state words' size, as on the reading pages; a waiting
 * line too wide for that (WARMING UP) steps down one.
 */
void envui_clock(canvas_t *c, const envui_clock_t *k)
{
    canvas_clear(c);
    if (k == NULL) return;
    int mid = c->w / 2;

    if (k->date != NULL && k->date[0] != '\0') {
        const aafont_t *f = aafont_width(F_SUB, k->date) <= XR - XL ? F_SUB : F_LABEL;
        int top = K_DATE_TOP + F_SUB->cap - f->cap;           /* on the same baseline */
        if (aafont_width(f, k->date) <= XR - XL)
            aafont_draw(c, f, mid, top, k->date, COL_WHITE, AAFONT_CENTRE);
        else
            aafont_draw(c, f, XL, top, k->date, COL_WHITE, AAFONT_LEFT);
    }

    if (k->time != NULL)
        aafont_draw(c, F_NUMBER, mid, K_TIME_TOP, k->time, COL_WHITE, AAFONT_CENTRE | AAFONT_ADVANCE);

    if (!k->air) return;
    float size = (float)F_STATE->cap;
    int top = K_VERDICT_TOP;
    if (verdict_has_word(k->verdict)) {
        float w = envui_verdict_width(size, k->verdict);
        envui_verdict(c, ((float)c->w - w) / 2.0f, (float)top, size, k->verdict);
        return;
    }
    if (envui_verdict_wait_width(size, k->gas_error) > (float)(XR - XL)) {
        size = (float)F_WORD->cap;
        top += F_STATE->cap - F_WORD->cap;                     /* on the same baseline */
    }
    float w = envui_verdict_wait_width(size, k->gas_error);
    envui_verdict_wait(c, ((float)c->w - w) / 2.0f, (float)top, size, k->gas_error);
}
