#include "envui.h"

#include "vector.h"
#include "vfont.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/*
 * The layouts are the spec's, measured off the research mockups
 * (docs/design/envo-ui/mock_rev1.c page_single, mock_rev2.c for the chart and
 * the week). Every y below is the top of the capitals' INK -- for the bitmap
 * font and the stroke font alike -- so two pieces of text said to line up do,
 * whichever font each is in.
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

/* Stroke weight as a share of the size: the big numbers a little lighter,
   so their counters stay open at 60 px; words at the mockups' 0.15. */
#define NUM_WT  0.13f
#define WORD_WT 0.15f

/* ---- the reading page -------------------------------------------------- */

#define LABEL_CAP    14.0f  /* the bitmap labels' capitals, in px */
#define R_LABEL_TOP  8
/* The number: as big as the page allows, ink 27..90 (63 px) at full size.
   It stands on a fixed ink bottom, so when a wide one shrinks it stays on
   the same line above its word. */
#define R_NUM_SIZE   56.0f
#define R_NUM_MIN    40.0f
#define R_NUM_MAXW   200.0f
#define R_NUM_BOTTOM 90.0f
#define R_NUM_GAP    10.0f  /* between the number and the trend word */
/* The state word under it, ink 98..130; POOR's block reaches 4 px beyond,
   94..134, clear of the number above and the strip below. */
#define R_WORD_SIZE  28.0f
#define R_WORD_TOP   98.0f
#define R_WORD_PAD   4.0f
#define R_NOTE_SIZE  22.0f  /* FROM VOCS, NO READING, the warm-up minutes */
#define R_NOTE_TOP   101.0f
/* The trend, right-aligned: the arrow at the top of the number's band, the
   word standing on the number's ink bottom. */
#define R_ARROW      26.0f
#define R_ARROW_TOP  30.0f
#define R_TREND_SIZE 22.0f
/* Warming up: the message where the number would be. */
#define R_WARM_SIZE  34.0f
#define R_WARM_TOP   30.0f
#define R_SOFAR_TOP  82.0f
/* The 24 h strip, rows 138..151 over a floor at 152, then its end labels
   with the page dots between them. */
#define R_STRIP_FLOOR 152
#define R_STRIP_H     14.0f
#define R_STRIP_LABEL 156
#define R_DOTS_Y      162.5f

/* ---- the full chart ------------------------------------------------------ */

/* The plot ends 10 px short of the key rather than 7, so the now-dot, 3.5 px
   in radius, is not read as a bullet on the key word level with it. */
#define D_X0     16
#define D_X1     246        /* 231 columns of about 6.2 minutes */
#define D_FLOOR  142        /* plot rows 44..141 */
#define D_H      98.0f
#define D_KEY_X  256
#define D_AXIS   150        /* the time axis labels */
/* The header, right: number (30, shrinking to 22) and word (22) stand on
   one baseline, ink bottoms at y 39, as the mockup has them; POOR's block
   then spans 12..42, clear of the key's POOR under it at 45. */
#define D_NUM_SIZE   30.0f
#define D_NUM_MIN    22.0f
#define D_NUM_BOTTOM 40.0f
#define D_WORD_SIZE  22.0f
#define D_WORD_TOP   14.0f
#define D_WORD_PAD   2.0f
#define D_ARROW      18.0f
#define D_ARROW_CY   26.0f

/* ---- the week ------------------------------------------------------------ */

/* Rows 16 px apart rather than the mockup's 17, so the hour labels finish at
   y 154 and the page dots fit under them at the same height as on every
   other page. The grid ends at x 304 exactly: 43 + 23 * 11 + 9. */
#define W_GX   43
#define W_GY   28
#define W_CW   11
#define W_CH   16
#define W_CELL_W 9
#define W_CELL_H 12

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
 * says it is an estimate, FROM VOCS says where from in the word's slot, and
 * "ppm, from VOCs" filled the top line from edge to edge, so that label and
 * unit ran together into one phrase. Its chart is titled "eCO2 24H" over
 * "ppm, est.": the header's number and word have to clear both lines, and
 * "est." keeps the estimate named on the one page that has no label.
 */
static const meta_t s_meta[ENVS_N] = {
    [ENVS_VOC]  = { "VOC",      "ppb", "VOC 24H",      "ppb",       0,    2200, { 0, 0 } },
    [ENVS_ECO2] = { "eCO2 est", "ppm", "eCO2 24H",     "ppm, est.", 400,  1500, { 0, 0 } },
    [ENVS_TEMP] = { "TEMP",     "C",   "TEMP 24H",     "C",         1600, 3000, { 2000, 2500 } },
    [ENVS_RH]   = { "HUMIDITY", "%",   "HUMIDITY 24H", "%",         2000, 8000, { 3000, 6000 } },
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

/* The bitmap labels are 1x whatever magnification the caller's canvas has;
   a copy is cheap and leaves theirs alone. */
static canvas_t at_1x(const canvas_t *c)
{
    canvas_t t;
    canvas_init(&t, c->fb, c->w, c->h, 1);
    return t;
}

/* The 12x24 bitmap font. Its capitals' ink is rows 4..17 of the cell. */
static void text(canvas_t *c, int x, int top, const char *s, uint16_t col)
{
    canvas_puts_px(c, x, top - 4, s, col);
}

static int text_w(const canvas_t *c, const char *s)
{
    return c->cell_w * (int)strlen(s);
}

/* The stroke font, placed by its ink: `x` is the ink's left edge, centre or
   right edge by `align`, `top` the top of the ink. vfont itself places the
   centre lines, and the stroke reaches half a weight past them. */
static float vwidth(float size, float wt, const char *s)
{
    vfont_style_t st = { size, wt, 0.0f };
    return vfont_width(&st, s) + wt;
}

static void vtext(canvas_t *c, float x, float top, float size, float wt,
                  int align, uint16_t col, const char *s)
{
    vfont_style_t st = { size, wt, 0.0f };
    float ax = align == VFONT_LEFT ? x + wt / 2 : align == VFONT_RIGHT ? x - wt / 2 : x;
    vfont_draw(c, &st, ax, top + wt / 2 + size / 2, 0.0f, align, col, s);
}

/*
 * A unit beside a label. The bitmap font's "%" is an x-height squiggle that
 * reads as "96" at arm's length, so a lone "%" is set in the stroke font at
 * the labels' cap height instead; everything else is bitmap text like the
 * label it sits by. Returns the ink width.
 */
static int unit(canvas_t *c, int x, int top, const char *s, int align, uint16_t col)
{
    if (strcmp(s, "%") == 0) {
        float wt = 2.0f, w = vwidth(LABEL_CAP, wt, s);
        vtext(c, (float)x, (float)top, LABEL_CAP, wt, align, col, s);
        return (int)ceilf(w);
    }
    int w = text_w(c, s);
    text(c, align == VFONT_RIGHT ? x - w : x, top, s, col);
    return w;
}

/* Floats to pixel edges, sanely even for a coordinate a caller got wrong:
   NaN and anything past the panel by miles land just off it. */
static int edge(float v)
{
    if (!(v > -30000.0f)) return -30000;
    if (v > 30000.0f) return 30000;
    return (int)floorf(v);
}

static void block(canvas_t *c, float x0, float y0, float x1, float y1, uint16_t col)
{
    int a = edge(x0), b = edge(y0), r = edge(ceilf(x1)), d = edge(ceilf(y1));
    canvas_fill_rect(c, a, b, r - a, d - b, col);
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

/* The width a state word takes, its block included. */
static float state_width(float size, envs_state_t st, float pad)
{
    const char *w = state_name(st);
    if (w == NULL) return 0.0f;
    return vwidth(size, size * WORD_WT, w) + (st == ENVS_POOR ? 2 * pad : 0.0f);
}

/*
 * A state's word, its left edge at x: GOOD in blue, FAIR in amber, POOR in
 * black on a red block reaching `pad` beyond the ink -- the one state that
 * must be seen from across the room is the one that is a solid patch.
 */
static void state_word(canvas_t *c, float x, float top, float size, envs_state_t st, float pad)
{
    const char *w = state_name(st);
    if (w == NULL) return;
    float wt = size * WORD_WT;
    if (st != ENVS_POOR) {
        vtext(c, x, top, size, wt, VFONT_LEFT, st == ENVS_OK ? COL_GOOD : COL_FAIR, w);
        return;
    }
    block(c, x, top - pad, x + state_width(size, st, pad), top + size + wt + pad, COL_POOR);
    vtext(c, x + pad, top, size, wt, VFONT_LEFT, COL_BG, w);
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

/* The degree sign, which vfont does not have: a ring at the top of the
   capitals, a little after the number. Returns the width it adds. */
static float degree_width(float size) { return size * 0.06f + size * 0.22f + size * 0.07f + 0.6f; }

static void degree(canvas_t *c, float x, float top, float size, uint16_t col)
{
    float r = size * 0.11f, w = size * 0.07f + 0.6f;
    vec_ring(c, x + size * 0.06f + r + w / 2, top + r + w / 2, r, w, col);
}

/*
 * What stands after a number: the degree ring for temperature, a percent sign
 * for humidity, or nothing. Humidity's gets one because the reading page's
 * "37" is otherwise a bare number whose unit is a 14 px grey "%" in the
 * corner -- from across the room it could as well be a temperature, and the
 * temperature beside it carries its own ring. Half the number's size, top to
 * top with it, the way a unit is set after a big figure.
 */
typedef enum { SUFFIX_NONE, SUFFIX_DEG, SUFFIX_PCT } suffix_t;

#define PCT_SCALE 0.5f
#define PCT_GAP   4.0f

static float suffix_width(float size, suffix_t sx)
{
    if (sx == SUFFIX_DEG) return degree_width(size);
    if (sx == SUFFIX_PCT) {
        float ps = size * PCT_SCALE;
        return PCT_GAP + vwidth(ps, ps * NUM_WT, "%");
    }
    return 0.0f;
}

/* A number in the stroke font, with its suffix; returns its whole ink width. */
static float number_width(const char *b, float size, float wt, suffix_t sx)
{
    return vwidth(size, wt, b) + suffix_width(size, sx);
}

static void number(canvas_t *c, float x, float top, float size, float wt, suffix_t sx,
                   uint16_t col, const char *b)
{
    vtext(c, x, top, size, wt, VFONT_LEFT, col, b);
    float r = x + vwidth(size, wt, b);
    if (sx == SUFFIX_DEG) degree(c, r, top, size, col);
    if (sx == SUFFIX_PCT) {
        float ps = size * PCT_SCALE;
        vtext(c, r + PCT_GAP, top, ps, ps * NUM_WT, VFONT_LEFT, col, "%");
    }
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
    int now_x = p->x1 + 1 - text_w(c, "NOW");
    text(c, now_x, D_AXIS, "NOW", COL_WHITE);

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
        int w = text_w(c, lab), lx = x - w / 2;
        if (lx < XL || lx + w > now_x - 4) continue;
        text(c, lx, D_AXIS, lab, is_mid && exact ? COL_WHITE : COL_GREY);
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
     * would otherwise run into it ("1100 POOR"). The knock-out is 16 rows,
     * one either side of the ink; if a zone line would run through it, the
     * number drops to just under that line rather than cutting it -- a zone
     * line with a hole in it is an edge the eye cannot follow across.
     */
    char b[16];
    format_value(s->series, s->peak_value, b, sizeof b);
    int w = text_w(c, b);
    int lx = (int)px + 7;
    if (lx + w > p->x1 - 12) lx = (int)px - 7 - w;
    int ly = (int)py - 16;
    if (ly < top) ly = top;
    int line[2] = { yline(p, s->series, L->poor), yline(p, s->series, L->fair) };
    for (int k = 0; k < 2; k++)
        if (line[k] >= ly - 1 && line[k] <= ly + 14) ly = line[k] + 3;
    canvas_fill_rect(c, lx - 2, ly - 1, w + 4, 16, COL_BG);
    text(c, lx, ly, b, COL_WHITE);
}

/* The key beside the full chart: each zone's word in its colour, and the
   two edges' values in grey level with their lines. */
static void key(canvas_t *c, const envui_series_t *s, const plot_t *p)
{
    char b[16];
    const envs_limits_t *L = is_gas(s->series) ? envs_limits(s->series) : NULL;
    if (L != NULL) {
        int yf = yline(p, s->series, L->fair), yp = yline(p, s->series, L->poor);
        text(c, D_KEY_X, p->floor - (int)p->h + 1, "POOR", COL_POOR);
        snprintf(b, sizeof b, "%ld", (long)L->poor);
        text(c, D_KEY_X, yp - 7, b, COL_GREY);
        text(c, D_KEY_X, (yf + yp) / 2 - 7, "FAIR", COL_FAIR);
        snprintf(b, sizeof b, "%ld", (long)L->fair);
        text(c, D_KEY_X, yf - 7, b, COL_GREY);
        /* eCO2 is never called GOOD: it is estimated from the VOCs, so a low
           one says nothing the VOC page has not. */
        if (s->series == ENVS_VOC) text(c, D_KEY_X, yf + 14, "GOOD", COL_GOOD);
        return;
    }
    const meta_t *m = &s_meta[s->series];
    for (int k = 0; k < 2; k++) {
        snprintf(b, sizeof b, "%ld", (long)(m->ref[k] / 100));
        text(c, D_KEY_X, yline(p, s->series, m->ref[k]) - 7, b, COL_GREY);
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
        const char *t = "NO DATA";
        int w = text_w(c, t), x = (p->x0 + p->x1 + 1) / 2 - w / 2, y = (ya + yb) / 2 - 7;
        text(c, x, y, t, COL_GREY);
    }
}

/* ---- the reading page ---------------------------------------------------- */

/* The trend, right-aligned: an arrow and RISING or FALLING in white, or
   STEADY in grey alone. Nothing until there is enough history to say.
   Returns the left edge of what it drew, for the number to keep clear of. */
static float reading_trend(canvas_t *c, envs_trend_t tr)
{
    float wt = R_TREND_SIZE * WORD_WT;
    const char *w = tr == ENVS_RISING ? "RISING" : tr == ENVS_FALLING ? "FALLING"
                  : tr == ENVS_STEADY ? "STEADY" : NULL;
    if (w == NULL) return XR + R_NUM_GAP;
    float top = R_NUM_BOTTOM - R_TREND_SIZE - wt;
    bool moving = tr != ENVS_STEADY;
    if (moving)
        arrow(c, XR - 0.4f * R_ARROW, R_ARROW_TOP + R_ARROW / 2, R_ARROW, tr == ENVS_RISING, COL_WHITE);
    vtext(c, XR, top, R_TREND_SIZE, wt, VFONT_RIGHT, moving ? COL_WHITE : COL_GREY, w);
    return XR - vwidth(R_TREND_SIZE, wt, w);
}

void envui_reading(canvas_t *cv, const envui_series_t *s, int page, int pages)
{
    canvas_t c = at_1x(cv);
    canvas_clear(&c);
    if (s == NULL || !series_ok(s->series)) return;
    const meta_t *m = &s_meta[s->series];

    text(&c, XL, R_LABEL_TOP, m->label, COL_GREY);
    unit(&c, XR, R_LABEL_TOP, m->unit, VFONT_RIGHT, COL_GREY);

    if (gas_waiting(s)) {
        /* No number while the chip settles: only how long it has been at it.
           Not under an error: the minutes count from the sensor's start, and
           beneath GAS ERROR a count going up reads as progress that is not
           being made. */
        vtext(&c, XL, R_WARM_TOP, R_WARM_SIZE, R_WARM_SIZE * WORD_WT, VFONT_LEFT, COL_GREY,
              wait_text(s->gas_error));
        if (!s->gas_error) {
            char b[24];
            so_far(b, sizeof b, s->warm_minutes);
            vtext(&c, XL, R_SOFAR_TOP, R_NOTE_SIZE, R_NOTE_SIZE * WORD_WT, VFONT_LEFT, COL_GREY, b);
        }
    } else if (!s->have_now) {
        /* A sensor that is not answering: say so, rather than leave a "--"
           that could as well mean "wait". */
        vtext(&c, XL, R_NUM_BOTTOM - R_NUM_SIZE * (1 + NUM_WT), R_NUM_SIZE, R_NUM_SIZE * NUM_WT,
              VFONT_LEFT, COL_GREY, "--");
        vtext(&c, XL, R_NOTE_TOP, R_NOTE_SIZE, R_NOTE_SIZE * WORD_WT, VFONT_LEFT, COL_GREY, "NO READING");
    } else {
        float trend_left = reading_trend(&c, s->trend);

        /* As big as fits: 200 px at most, and clear of the trend word. */
        char b[16];
        format_value(s->series, s->now_value, b, sizeof b);
        suffix_t sx = s->series == ENVS_TEMP ? SUFFIX_DEG : s->series == ENVS_RH ? SUFFIX_PCT : SUFFIX_NONE;
        float size = R_NUM_SIZE, wt, w;
        for (;;) {
            wt = size * NUM_WT;
            w = number_width(b, size, wt, sx);
            if ((w <= R_NUM_MAXW && XL + w + R_NUM_GAP <= trend_left) || size <= R_NUM_MIN) break;
            size -= 2.0f;
        }
        number(&c, XL, R_NUM_BOTTOM - size - wt, size, wt, sx, COL_WHITE, b);

        /* POOR's block starts its pad left of the margin, so its letters line
           up with the number's at x 16 rather than sitting indented under
           it: the block is not text, and nothing there is near a corner. */
        if (s->series == ENVS_ECO2 && s->state == ENVS_OK)
            /* eCO2 is never GOOD: below its first limit it says where it comes from. */
            vtext(&c, XL, R_NOTE_TOP, R_NOTE_SIZE, R_NOTE_SIZE * WORD_WT, VFONT_LEFT, COL_GREY, "FROM VOCS");
        else if (is_gas(s->series))
            state_word(&c, s->state == ENVS_POOR ? XL - R_WORD_PAD : XL, R_WORD_TOP,
                       R_WORD_SIZE, s->state, R_WORD_PAD);
    }

    plot_t p = { XL, XR, R_STRIP_FLOOR, R_STRIP_H, 1.6f, 2.5f, false };
    plot(&c, s, &p);
    text(&c, XL, R_STRIP_LABEL, "-24H", COL_GREY);
    text(&c, XR - text_w(&c, "NOW"), R_STRIP_LABEL, "NOW", COL_GREY);
    page_dots(&c, page, pages);
}

/* ---- the full chart ------------------------------------------------------ */

/*
 * The header's right side: the word at the right edge, then the arrow, then
 * the number, which steps down from 30 to 22 px to keep 12 px clear of the
 * title. If it still cannot, the arrow goes -- the reading page behind this
 * one shows the trend in words anyway.
 */
static void detail_header(canvas_t *c, const envui_series_t *s, int title_r)
{
    /* No number to give: why not, in words, in grey. NO READING rather than a
       "--", which at this size is a 4 px smudge that says nothing. */
    if (gas_waiting(s) || !s->have_now) {
        const char *msg = gas_waiting(s) ? wait_text(s->gas_error) : "NO READING";
        float size = D_WORD_SIZE;
        while (size > 12.0f && XR - vwidth(size, size * WORD_WT, msg) < (float)title_r) size -= 1.0f;
        float wt = size * WORD_WT;
        vtext(c, XR, D_ARROW_CY - (size + wt) / 2, size, wt, VFONT_RIGHT, COL_GREY, msg);
        return;
    }

    float right = XR;
    bool word = is_gas(s->series) && state_name(s->state) != NULL
             && !(s->series == ENVS_ECO2 && s->state == ENVS_OK);
    if (word) {
        float pad = s->state == ENVS_POOR ? D_WORD_PAD : 0.0f;
        float x = XR - state_width(D_WORD_SIZE, s->state, pad);
        state_word(c, x, D_WORD_TOP, D_WORD_SIZE, s->state, pad);
        right = x - 10.0f;
    }

    /* The degree ring stays, being part of how a temperature is written; the
       percent sign does not, the subtitle beside it already saying "%". */
    char b[16];
    format_value(s->series, s->now_value, b, sizeof b);
    suffix_t sx = s->series == ENVS_TEMP ? SUFFIX_DEG : SUFFIX_NONE;
    bool moving = s->trend == ENVS_RISING || s->trend == ENVS_FALLING;
    for (int pass = moving ? 0 : 1; pass < 2; pass++) {
        float r = pass == 0 ? right - D_ARROW * 0.8f - 10.0f : right;
        float size = D_NUM_SIZE, wt, w;
        for (;;) {
            wt = size * WORD_WT;
            w = number_width(b, size, wt, sx);
            if (r - w >= (float)title_r || size <= D_NUM_MIN) break;
            size -= 2.0f;
        }
        if (pass == 0 && r - w < (float)title_r) continue;
        if (pass == 0)
            arrow(c, right - D_ARROW * 0.4f, D_ARROW_CY, D_ARROW, s->trend == ENVS_RISING, COL_WHITE);
        number(c, r - w, D_NUM_BOTTOM - size - wt, size, wt, sx, COL_WHITE, b);
        break;
    }
}

void envui_detail(canvas_t *cv, const envui_series_t *s)
{
    canvas_t c = at_1x(cv);
    canvas_clear(&c);
    if (s == NULL || !series_ok(s->series)) return;
    const meta_t *m = &s_meta[s->series];

    /* The header's right side keeps 12 px clear of the longer of the two
       title lines: its number is tall enough to span both. */
    text(&c, XL, 8, m->title, COL_WHITE);
    int tw = text_w(&c, m->title), sw = unit(&c, XL, 24, m->subtitle, VFONT_LEFT, COL_GREY);
    detail_header(&c, s, XL + (tw > sw ? tw : sw) + 12);

    plot_t p = { D_X0, D_X1, D_FLOOR, D_H, 2.0f, 3.5f, true };
    plot(&c, s, &p);
}

/* ---- the week ------------------------------------------------------------ */

/*
 * Seven days by 24 hours, each hour the worst state that lasted in it (the
 * caller decides what lasted). Ink is for exceptions: an OK hour is a thin
 * blue bar, FAIR half a cell of amber, POOR the whole cell in red -- so the
 * height of a mark says how bad before its colour does.
 */
void envui_week(canvas_t *cv, const envui_week_t *w, int page, int pages)
{
    canvas_t c = at_1x(cv);
    canvas_clear(&c);
    if (w == NULL) return;

    text(&c, XL, 8, "WEEK", COL_WHITE);

    bool any = false;
    for (int d = 0; d < 7; d++) {
        char name[3] = { w->day[d][0], w->day[d][1], '\0' };
        text(&c, XL, W_GY + d * W_CH, name, d == 6 ? COL_WHITE : COL_GREY);
        for (int h = 0; h < 24; h++) {
            int x = W_GX + h * W_CW, y = W_GY + d * W_CH + 1;
            switch (w->cell[d][h]) {
            case ENVUI_CELL_NONE:
                canvas_fill_rect(&c, x + W_CELL_W / 2 - 1, y + W_CELL_H / 2 - 1, 2, 2, COL_RULE);
                break;
            case ENVUI_CELL_OK:
                canvas_fill_rect(&c, x, y + W_CELL_H - 3, W_CELL_W, 3, COL_GOOD);
                any = true;
                break;
            case ENVUI_CELL_FAIR:
                canvas_fill_rect(&c, x, y + W_CELL_H - 7, W_CELL_W, 7, COL_FAIR);
                any = true;
                break;
            case ENVUI_CELL_POOR:
                canvas_fill_rect(&c, x, y, W_CELL_W, W_CELL_H, COL_POOR);
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
        const char *t = any ? "ALL GOOD" : "NO DATA";
        text(&c, XR - text_w(&c, t), 8, t, any ? COL_GOOD : COL_GREY);
    } else {
        int x = XR;
        if (fair > 0) {
            snprintf(b, sizeof b, "FAIR %dH", fair);
            x -= text_w(&c, b);
            text(&c, x, 8, b, COL_FAIR);
            x -= 24;
        }
        if (poor > 0) {
            snprintf(b, sizeof b, "POOR %dH", poor);
            text(&c, x - text_w(&c, b), 8, b, COL_POOR);
        }
    }

    /* 00 at the grid's left, 06/12/18 centred on the gaps before those
       hours, 24 at its right. Their ink is 141..154: 3 px under the last
       row's cells and 5 px over the page dots, so "12" reads as a label on
       the grid and not as a caption on the dots under it. */
    int ly = W_GY + 7 * W_CH + 1;
    text(&c, W_GX, ly, "00", COL_GREY);
    const char *mid[] = { "06", "12", "18" };
    for (int k = 0; k < 3; k++) {
        int x = W_GX + (k + 1) * 6 * W_CW - 1;
        text(&c, x - text_w(&c, mid[k]) / 2, ly, mid[k], COL_GREY);
    }
    text(&c, XR + 1 - text_w(&c, "24"), ly, "24", COL_GREY);

    page_dots(&c, page, pages);
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
    return v.state == ENVS_OK ? "AIR" : v.worst == ENVS_ECO2 ? "ECO2" : "VOC";
}

/* A size the stroke font can draw, and a place near enough the panel to be
   worth drawing at; anything else draws nothing rather than a stroke a
   million pixels long. */
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

void envui_verdict(canvas_t *cv, float x, float y, float size, envs_verdict_t v)
{
    if (!verdict_has_word(v)) {
        envui_verdict_wait(cv, x, y, size, false);
        return;
    }
    if (!verdict_size(&size) || !verdict_place(x, y)) return;
    canvas_t c = at_1x(cv);
    float wt = size * WORD_WT;
    const char *who = verdict_who(v);
    vtext(&c, x, y, size, wt, VFONT_LEFT, COL_WHITE, who);
    float pad = v.state == ENVS_POOR ? size * 0.2f : 0.0f;
    state_word(&c, x + vwidth(size, wt, who) + size * 0.6f - pad, y, size, v.state, pad);
}

/* The same line's width, worked out the way envui_verdict lays it out, so the
   clock can centre it under its digits without knowing the stroke font's
   metrics. */
float envui_verdict_width(float size, envs_verdict_t v)
{
    if (!verdict_has_word(v)) return envui_verdict_wait_width(size, false);
    if (!verdict_size(&size)) return 0.0f;
    float wt = size * WORD_WT;
    float pad = v.state == ENVS_POOR ? size * 0.2f : 0.0f;
    return vwidth(size, wt, verdict_who(v)) + size * 0.6f - pad
         + state_width(size, v.state, pad);
}

/*
 * No verdict yet, and why, in grey: the chip warming up, or the chip saying
 * its data is invalid. The two must not share a word -- a clock that said
 * WARMING UP while the VOC page one swipe away said GAS ERROR would be
 * contradicting it, and warming up promises a verdict an error never brings.
 */
void envui_verdict_wait(canvas_t *cv, float x, float y, float size, bool gas_error)
{
    if (!verdict_size(&size) || !verdict_place(x, y)) return;
    canvas_t c = at_1x(cv);
    vtext(&c, x, y, size, size * WORD_WT, VFONT_LEFT, COL_GREY, wait_text(gas_error));
}

float envui_verdict_wait_width(float size, bool gas_error)
{
    if (!verdict_size(&size)) return 0.0f;
    return vwidth(size, size * WORD_WT, wait_text(gas_error));
}
