#include "daysui.h"

#include "aafont.h"
#include "noiseui.h"
#include "ring.h"
#include "vector.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Every y below is the top of a line's capitals, as in noiseui and envui;
 * where two sizes share a line, the line is a baseline -- the top plus the
 * face's cap.
 */

/* ---- palette ---------------------------------------------------------------
 * The Sound page's, which is the watch face's: cream for what is printed, a
 * softer cream for units, gold for labels, a blue-grey rail, the hatch for a
 * day with no figure. The level's colour -- noiseui_colour, so the two pages
 * and speaker's ring are one colour for one level -- is the only other. */
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define CREAM       RGB(236, 228, 206)
#define CREAM_SOFT  RGB(196, 190, 172)
#define CREAM_DIM   RGB(140, 136, 122)
#define GOLD        RGB(224, 186, 118)
#define GOLD_LO     RGB(156, 116, 60)
#define RAIL        RGB(84, 90, 108)
#define HATCH       RGB(52, 58, 74)

/* The ground: the Sound page's night sky, the same numbers, so turning from
   one page to the other changes what is on it and nothing under it. */
static const float s_sky_top[3] = { 9.0f, 14.0f, 34.0f };
static const float s_sky_foot[3] = { 3.0f, 5.0f, 13.0f };

/* ---- layout ---------------------------------------------------------------
 * The panel's rounded corners as on the Sound page: nothing within 12 px of
 * the sides, nothing within 8 px of a corner's arc. The bars are laid out
 * first, from the foot -- they need a row a decibel to show a 3 dB day as
 * 3 px, and a figure over each -- and the lines above them fitted into what
 * is left. So today's figure is a size under the Sound page's: that one is
 * read across the room, this one with the words under it. */
#define MID         120

#define CAP_TOP     14             /* TODAY SO FAR, capitals 14..26 */
#define NUM_BASE    83             /* the figure's capitals 36..82 */
#define UNIT_GAP    7
#define NO_DATA_TOP 54             /* NO DATA, in the figure's place */
#define DELTA_BASE  119            /* +3.2 (capitals 94..118) and "dB louder" stand on this */
#define THAN_TOP    128            /* than a usual Friday (4), capitals 128..140 */
#define BG_TOP      155            /* BACKGROUND 36 dBA, capitals 155..167 */
#define TEXT_W      212            /* the most a centred line may take */

/* The bars: seven slots of 30 px across the page, over 50 rows, one per dB
   from 30 to 80, each day's figure over its bar -- the scale's numerals, as
   the Sound page has 45 and 75, would crowd a bar that is all of 30 px --
   and Mo..Su under the floor. The tallest bar's figure clears BACKGROUND by
   five clear rows. */
#define B_X0        15
#define B_COLS      210
#define B_FLOOR     240            /* the floor row; the plot is rows 190..239 */
#define B_ROWS      50
#define B_LO        30.0f
#define B_HI        80.0f
#define B_BAR_W     18
#define B_CAP       2.0f           /* the bar's top, in its full colour */
#define B_FIG_GAP   4              /* a bar's top to its figure's foot */
#define B_LABEL_TOP 247            /* Mo..Su, capitals 247..259 */

#define F_LABEL     (&aafont_inter_label)     /* capitals 13 */
#define F_SUB       (&aafont_inter_sub)       /* 17 */
#define F_WORD      (&aafont_inter_word)      /* 25 */
#define F_NUMBER    (&aafont_inter_number)    /* 47 */

/* ---- dates ------------------------------------------------------------------- */

static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static bool date_ok(int y, int m, int d)
{
    static const uint8_t dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (y < 2000 || y > 2199 || m < 1 || m > 12 || d < 1) return false;
    return d <= dim[m - 1] + (m == 2 && leap(y) ? 1 : 0);
}

/* Days since 1970-01-01, by the civil calendar (Howard Hinnant's
   days_from_civil); for dates date_ok passes, so no negative years. */
static long day_number(int y, int m, int d)
{
    y -= m <= 2;
    long era = y / 400;
    long yoe = y - era * 400;
    long doy = (153L * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int daysui_weekday(int year, int month, int day)
{
    if (!date_ok(year, month, day)) return -1;
    /* 1970-01-01 was a Thursday, 3 counting from Monday. */
    return (int)((day_number(year, month, day) % 7 + 7 + 3) % 7);
}

static bool day_dated(const daysui_day_t *d) { return date_ok(d->year, d->month, d->day); }
static long day_dn(const daysui_day_t *d) { return day_number(d->year, d->month, d->day); }

/* ---- levels ------------------------------------------------------------------ */

/* A level worth using: a finite number of dBA in a range a room can have.
   Anything else is "--": no figure, no bar, no part of a baseline -- and no
   1e9 dBA day turning an energy mean into infinity. */
static bool level_ok(float v) { return isfinite(v) && v >= 0.0f && v <= 200.0f; }

float daysui_energy_mean(const float *dba, int n)
{
    if (dba == NULL || n <= 0) return NAN;
    double e = 0.0;
    for (int i = 0; i < n; i++) e += pow(10.0, (double)dba[i] / 10.0);
    return (float)(10.0 * log10(e / (double)n));
}

static int count(const daysui_t *s)
{
    return s->n < 0 ? 0 : s->n > DAYSUI_DAYS ? DAYSUI_DAYS : s->n;
}

/* Today: the last day flagged today that has a date; -1 when none is. */
static int today_index(const daysui_t *s)
{
    for (int i = count(s) - 1; i >= 0; i--)
        if (s->day[i].today && day_dated(&s->day[i])) return i;
    return -1;
}

/*
 * The day dated latest strictly before day number `below`, -1 when there is
 * none. Walking back with this -- each step below the last -- visits the
 * earlier days newest first and each date once, however main.c has ordered
 * them or doubled one up.
 */
static int latest_before(const daysui_t *s, long below)
{
    int best = -1;
    long bdn = LONG_MIN;
    for (int i = 0; i < count(s); i++) {
        const daysui_day_t *d = &s->day[i];
        if (!day_dated(d)) continue;
        long dn = day_dn(d);
        if (dn < below && (best < 0 || dn > bdn)) { best = i; bdn = dn; }
    }
    return best;
}

/* Where the walk back starts: today's date, or past everything with no today. */
static long today_dn(const daysui_t *s)
{
    int t = today_index(s);
    return t >= 0 ? day_dn(&s->day[t]) : LONG_MAX;
}

daysui_base_t daysui_baseline(const daysui_t *s)
{
    daysui_base_t b = { DAYSUI_BASE_NONE, NAN, 0, -1 };
    if (s == NULL) return b;
    int t = today_index(s);
    if (t >= 0) b.weekday = daysui_weekday(s->day[t].year, s->day[t].month, s->day[t].day);

    /* Same weekday first: seven days apart, so up to four in five weeks. */
    float v[7];
    int n = 0;
    if (b.weekday >= 0) {
        for (long below = today_dn(s); n < 4;) {
            int i = latest_before(s, below);
            if (i < 0) break;
            const daysui_day_t *d = &s->day[i];
            below = day_dn(d);
            if (daysui_weekday(d->year, d->month, d->day) == b.weekday && level_ok(d->laeq))
                v[n++] = d->laeq;
        }
        if (n > 0) {
            b.kind = DAYSUI_BASE_WEEKDAY;
            b.level = daysui_energy_mean(v, n);
            b.n = n;
            return b;
        }
    }

    /* None of those yet: the latest days of any weekday. */
    for (long below = today_dn(s); n < 7;) {
        int i = latest_before(s, below);
        if (i < 0) break;
        below = day_dn(&s->day[i]);
        if (level_ok(s->day[i].laeq)) v[n++] = s->day[i].laeq;
    }
    if (n > 0) {
        b.kind = DAYSUI_BASE_RECENT;
        b.level = daysui_energy_mean(v, n);
        b.n = n;
    }
    return b;
}

float daysui_background(const daysui_t *s, int *np)
{
    if (np != NULL) *np = 0;
    if (s == NULL) return NAN;
    float v[7];
    int n = 0;
    for (long below = today_dn(s); n < 7;) {
        int i = latest_before(s, below);
        if (i < 0) break;
        below = day_dn(&s->day[i]);
        if (!level_ok(s->day[i].l90)) continue;
        /* Kept sorted as it fills, for the median. */
        int k = n++;
        while (k > 0 && v[k - 1] > s->day[i].l90) { v[k] = v[k - 1]; k--; }
        v[k] = s->day[i].l90;
    }
    if (n > 0) {
        if (np != NULL) *np = n;
        return n % 2 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
    }
    int t = today_index(s);
    return t >= 0 && level_ok(s->day[t].l90) ? s->day[t].l90 : NAN;
}

/* A level as the page prints it: whole decibels, rounded, as the Sound page
   rounds its own -- one day's figure and the other's agree to the dB. */
static int whole_db(float v)
{
    if (v <= 0.0f) return 0;
    if (v >= 199.0f) return 199;
    return (int)floorf(v + 0.5f);
}

/* ---- drawing helpers --------------------------------------------------------- */

static void pixel(canvas_t *c, int x, int y, uint16_t col)
{
    if (x >= 0 && y >= 0 && x < c->w && y < c->h) c->fb[y * c->w + x] = col;
}

static void tint(canvas_t *c, int x, int y, uint16_t col, float a)
{
    if (x < 0 || y < 0 || x >= c->w || y >= c->h || !(a > 0.0f)) return;
    uint16_t *p = &c->fb[y * c->w + x];
    *p = vec_blend_linear(*p, col, (uint8_t)(a >= 1.0f ? 255 : (int)(a * 255.0f + 0.5f)));
}

/* The level's colour with its light scaled by `k`, as the Sound page dims its
   stop lines: the ring's linear light, scaled, then encoded. */
static uint16_t colour_dim(float dba, float k)
{
    float lin[3];
    ring_colour_for(dba, lin);
    int b[3];
    for (int m = 0; m < 3; m++) {
        float v = lin[m] * k;
        v = v > 0.0f ? powf(v, 1.0f / 2.2f) * 255.0f : 0.0f;
        b[m] = v >= 255.0f ? 255 : (int)(v + 0.5f);
    }
    return RGB(b[0], b[1], b[2]);
}

static const uint8_t s_bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

/* noiseui.c's ground, line for line: the navy eased to near black, dithered. */
static void ground(canvas_t *c)
{
    for (int j = 0; j < c->h; j++) {
        float t = c->h > 1 ? (float)j / (float)(c->h - 1) : 0.0f;
        t = t * t * (3.0f - 2.0f * t);
        float ch[3];
        for (int k = 0; k < 3; k++) ch[k] = s_sky_top[k] + (s_sky_foot[k] - s_sky_top[k]) * t;
        uint16_t *row = c->fb + (size_t)j * (size_t)c->w;
        for (int i = 0; i < c->w; i++) {
            float d = ((float)s_bayer[((j & 3) << 2) | (i & 3)] + 0.5f) / 16.0f;
            int r5 = (int)(ch[0] / 255.0f * 31.0f + d);
            int g6 = (int)(ch[1] / 255.0f * 63.0f + d);
            int b5 = (int)(ch[2] / 255.0f * 31.0f + d);
            row[i] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        }
    }
}

/* noiseui.c's spaced capitals: `track` px between letters over the face's
   own spacing, placed by the ink; returns the ink's width, and draws only
   when `c` is not NULL. */
static int tracked(canvas_t *c, const aafont_t *f, int x, int y, const char *s,
                   uint16_t col, int align, int track)
{
    int pen = 0, left = 0, right = 0;
    bool any = false;
    for (const char *p = s; *p != '\0'; p++) {
        char one[2] = { *p, '\0' };
        int w = aafont_width(f, one);
        if (w > 0) {
            int l = pen + aafont_bearing(f, one);
            if (!any || l < left) left = l;
            if (!any || l + w > right) right = l + w;
            any = true;
        }
        pen += aafont_advance(f, one) + track;
    }
    if (!any) return 0;
    int width = right - left;
    if (c == NULL) return width;
    int x0 = align == AAFONT_CENTRE ? x - width / 2 : align == AAFONT_RIGHT ? x - width : x;
    pen = x0 - left;
    for (const char *p = s; *p != '\0'; p++) {
        char one[2] = { *p, '\0' };
        aafont_draw(c, f, pen, y, one, col, AAFONT_LEFT | AAFONT_ADVANCE);
        pen += aafont_advance(f, one) + track;
    }
    return width;
}

/* ---- today ------------------------------------------------------------------- */

static const char *const s_weekday_name[7] = {
    "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday",
};
static const char *const s_weekday_abbr[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
static const char *const s_weekday_short[7] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };

/*
 * The line saying what a same-weekday baseline is: "than a usual Friday (4)"
 * under a difference, "usual Friday: 47 dBA (4)" with no figure for today to
 * take one from. The weekday in full, or in three letters where the full name
 * will not fit -- "than a usual Wednesday (4)" is 226 px.
 */
static void weekday_line(char *out, size_t size, const daysui_base_t *b, bool with_level)
{
    for (int pass = 0; pass < 2; pass++) {
        const char *wd = b->weekday < 0 || b->weekday > 6 ? "day"
                       : pass == 0 ? s_weekday_name[b->weekday] : s_weekday_abbr[b->weekday];
        if (with_level) snprintf(out, size, "usual %s: %d dBA (%d)", wd, whole_db(b->level), b->n);
        else snprintf(out, size, "than a usual %s (%d)", wd, b->n);
        if (aafont_width(F_LABEL, out) <= TEXT_W) return;
    }
}

/*
 * Today's level so far and its unit, centred as one group as the Sound page
 * sets its level: the figure by its advance box (tabular digits, so it does
 * not shuffle as it changes), then a column with EST over dBA -- dBA on the
 * figure's baseline, EST level with its top -- while speaker is estimating.
 */
static void figure(canvas_t *c, const daysui_t *s, const daysui_day_t *t)
{
    bool ok = t != NULL && level_ok(t->laeq);
    char b[8];
    if (ok) snprintf(b, sizeof b, "%d", whole_db(t->laeq));
    else snprintf(b, sizeof b, "--");
    const aafont_t *f = F_NUMBER;
    int adv = aafont_advance(f, b), unit = aafont_width(F_SUB, "dBA");
    int x = MID - (adv + UNIT_GAP + unit) / 2;
    uint16_t col = !ok ? CREAM_DIM : s->stale ? colour_dim(t->laeq, 0.28f) : noiseui_colour(t->laeq);
    aafont_draw(c, f, x, NUM_BASE - f->cap, b, col, AAFONT_LEFT | AAFONT_ADVANCE);

    int ux = x + adv + UNIT_GAP;
    aafont_draw(c, F_SUB, ux, NUM_BASE - F_SUB->cap, "dBA", s->stale ? CREAM_DIM : CREAM_SOFT, AAFONT_LEFT);
    if (!s->calibrated)
        tracked(c, F_LABEL, ux, NUM_BASE - f->cap, "EST", s->stale ? CREAM_DIM : GOLD, AAFONT_LEFT, 1);
}

/*
 * The comparison, two lines: how far today is from the baseline, and what
 * the baseline is. The difference is a figure in the word face -- whose "-"
 * is a true minus, on the plus's axis, where the text faces' is a hyphen --
 * with its words in the size under it, on one baseline and centred together.
 */
static void comparison(canvas_t *c, const daysui_t *s, const daysui_day_t *t)
{
    daysui_base_t b = daysui_baseline(s);
    bool ok = t != NULL && level_ok(t->laeq);
    uint16_t hi = s->stale ? CREAM_DIM : CREAM, lo = s->stale ? CREAM_DIM : CREAM_SOFT;
    char than[48];

    if (b.kind == DAYSUI_BASE_NONE) {
        aafont_draw(c, F_SUB, MID, DELTA_BASE - F_SUB->cap, "first day", lo, AAFONT_CENTRE);
        aafont_draw(c, F_LABEL, MID, THAN_TOP, "baseline starts tomorrow", CREAM_DIM, AAFONT_CENTRE);
        return;
    }
    if (!ok) {
        /* No figure for today yet: say what a usual day is instead. */
        aafont_draw(c, F_SUB, MID, DELTA_BASE - F_SUB->cap, "nothing yet today", lo, AAFONT_CENTRE);
        if (b.kind == DAYSUI_BASE_WEEKDAY)
            weekday_line(than, sizeof than, &b, true);
        else
            snprintf(than, sizeof than, "recent days: %d dBA (%d)", whole_db(b.level), b.n);
        aafont_draw(c, F_LABEL, MID, THAN_TOP, than, lo, AAFONT_CENTRE);
        return;
    }

    float d = t->laeq - b.level;
    if (fabsf(d) < 1.0f) {
        aafont_draw(c, F_SUB, MID, DELTA_BASE - F_SUB->cap, "about the same", hi, AAFONT_CENTRE);
    } else {
        char fig[12];
        float m = fabsf(d) > 99.9f ? 99.9f : fabsf(d);
        snprintf(fig, sizeof fig, "%c%.1f", d > 0.0f ? '+' : '-', (double)m);
        const char *words = d > 0.0f ? "dB louder" : "dB quieter";
        int fw = aafont_width(F_WORD, fig), gap = 7, ww = aafont_width(F_SUB, words);
        int x = MID - (fw + gap + ww) / 2;
        aafont_draw(c, F_WORD, x, DELTA_BASE - F_WORD->cap, fig, hi, AAFONT_LEFT);
        aafont_draw(c, F_SUB, x + fw + gap, DELTA_BASE - F_SUB->cap, words, lo, AAFONT_LEFT);
    }
    if (b.kind == DAYSUI_BASE_WEEKDAY)
        weekday_line(than, sizeof than, &b, false);
    else
        snprintf(than, sizeof than, "than recent days (%d)", b.n);
    aafont_draw(c, F_LABEL, MID, THAN_TOP, than, lo, AAFONT_CENTRE);
}

/* BACKGROUND and its level, set as the Sound page sets TODAY: the label in
   gold, spaced, then the figure and its unit, centred as a group. */
static void background(canvas_t *c, const daysui_t *s)
{
    float v = daysui_background(s, NULL);
    bool ok = level_ok(v);
    char b[8];
    if (ok) snprintf(b, sizeof b, "%d", whole_db(v));
    else snprintf(b, sizeof b, "--");
    int lw = tracked(NULL, F_LABEL, 0, 0, "BACKGROUND", GOLD, AAFONT_LEFT, 2);
    int vw = aafont_width(F_LABEL, b), uw = ok ? aafont_width(F_LABEL, "dBA") : 0;
    int gap = 9, ugap = ok ? 5 : 0;
    int x = MID - (lw + gap + vw + ugap + uw) / 2;
    tracked(c, F_LABEL, x, BG_TOP, "BACKGROUND", GOLD, AAFONT_LEFT, 2);
    x += lw + gap;
    aafont_draw(c, F_LABEL, x, BG_TOP, b, ok ? CREAM : CREAM_DIM, AAFONT_LEFT);
    if (ok) aafont_draw(c, F_LABEL, x + vw + ugap, BG_TOP, "dBA", CREAM_SOFT, AAFONT_LEFT);
}

/* ---- the bars ---------------------------------------------------------------- */

/* Height above the floor, in px, on the fixed 30..80 scale. */
static float bar_h(float dba)
{
    float f = (dba - B_LO) / (B_HI - B_LO);
    if (!(f > 0.0f)) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f * (float)B_ROWS;
}

/* The day in the struct dated `dn`, -1 when there is none. */
static int day_on(const daysui_t *s, long dn)
{
    for (int i = count(s) - 1; i >= 0; i--)
        if (day_dated(&s->day[i]) && day_dn(&s->day[i]) == dn) return i;
    return -1;
}

/*
 * The last seven days by the calendar -- today and the six before it, each
 * looked up by its date, so a day main.c never heard of is a gap in its
 * place rather than the week shuffling left. Each bar in the colour of its
 * LAeq, faded towards the floor as the Sound page's wash fades and capped in
 * its full colour, as that page's line tops its wash. A day with no figure
 * after the first day that has one is hatched; before it, nothing -- the
 * week before speaker was plugged in is not a week of failures.
 */
static void bars(canvas_t *c, const daysui_t *s)
{
    int top = B_FLOOR - B_ROWS;
    /* A week by default, each bar with its figure and name; more than a week
       and the bars narrow to fill the same width, with only today's figure
       over them and a name under today and each same weekday before it --
       the days the comparison draws on. */
    const int nb = s->bars > DAYSUI_BARS && s->bars <= DAYSUI_DAYS ? s->bars : DAYSUI_BARS;
    const bool week = nb == DAYSUI_BARS;
    /* Narrow bars stop 12 px short, so today's name and lozenge under the
       last one keep out of watch's rounded corner. */
    const int cols = week ? B_COLS : B_COLS - 12;
    int bw = week ? B_BAR_W : cols / nb - 2;
    if (bw < 2) bw = 2;

    /* The ring's four stops, faint and dotted in their own colours as on
       the Sound page, but only in the gutters between the bars: across a
       slot they ran through the day's figure over it. */
    static const float stop[4] = { 45.0f, 55.0f, 65.0f, 75.0f };
    for (int k = 0; k < 4; k++) {
        int y = B_FLOOR - (int)lroundf(bar_h(stop[k]));
        uint16_t col = colour_dim(stop[k], 0.30f);
        for (int x = B_X0; x < B_X0 + cols; x += 3) {
            float at = ((float)(x - B_X0) + 0.5f) * (float)nb / (float)cols;
            float off = fabsf(at - floorf(at) - 0.5f) * (float)cols / (float)nb;
            if (off > (float)bw / 2.0f + 1.0f) pixel(c, x, y, col);
        }
    }
    canvas_fill_rect(c, B_X0, B_FLOOR, B_COLS, 1, RAIL);

    /* The last day: today, or with no today the latest dated day. */
    long last = LONG_MIN;
    int t = today_index(s);
    if (t >= 0) last = day_dn(&s->day[t]);
    else
        for (int i = 0; i < count(s); i++)
            if (day_dated(&s->day[i]) && day_dn(&s->day[i]) > last) last = day_dn(&s->day[i]);
    if (last == LONG_MIN) return;

    /* The first day speaker had a figure for, up to the last. */
    long first = LONG_MAX;
    for (int i = 0; i < count(s); i++) {
        const daysui_day_t *d = &s->day[i];
        if (day_dated(d) && level_ok(d->laeq) && day_dn(d) <= last && day_dn(d) < first) first = day_dn(d);
    }

    for (int k = 0; k < nb; k++) {
        long dn = last - (nb - 1) + k;
        float cx = (float)B_X0 + ((float)k + 0.5f) * (float)cols / (float)nb;
        int x0 = (int)lroundf(cx - (float)bw / 2.0f);
        bool named = week || (nb - 1 - k) % 7 == 0;
        bool is_today = t >= 0 && dn == last;
        bool dim = is_today && s->stale;
        int i = day_on(s, dn);
        const daysui_day_t *d = i >= 0 ? &s->day[i] : NULL;
        int wd = (int)((dn % 7 + 7 + 3) % 7);

        if (d != NULL && level_ok(d->laeq)) {
            /*
             * The wash, a fixed gradient by the row's height on the scale as
             * the Sound page's is, so a loud day is brighter as well as
             * taller; then the top B_CAP px in the full colour, each row by
             * how much of it the band covers, so a bar's top is where its
             * level is to a fraction of a pixel.
             */
            float h = bar_h(d->laeq);
            uint16_t col = dim ? colour_dim(d->laeq, 0.28f) : noiseui_colour(d->laeq);
            for (int y = B_FLOOR - 1; y >= top; y--) {
                float lo = (float)(B_FLOOR - 1 - y), hi = lo + 1.0f;   /* the row, as heights */
                if (lo >= h) break;
                float wash = fminf(hi, h) - lo;
                float cap = fminf(hi, h) - fmaxf(lo, h - B_CAP);
                float a = 0.22f + 0.40f * (lo + 0.5f) / (float)B_ROWS;
                for (int x = x0; x < x0 + bw; x++) {
                    tint(c, x, y, col, wash * a);
                    if (cap > 0.0f) tint(c, x, y, col, cap);
                }
            }
            if (week || is_today) {
                char b[8];
                snprintf(b, sizeof b, "%d", whole_db(d->laeq));
                int fy = B_FLOOR - (int)ceilf(h) - B_FIG_GAP - F_LABEL->cap;
                uint16_t fc = is_today ? (dim ? CREAM_DIM : CREAM) : CREAM_SOFT;
                /* Over narrow bars today's figure keeps inside the plot's right edge. */
                int fx = (int)lroundf(cx);
                if (!week) {
                    int half = aafont_width(F_LABEL, b) / 2;
                    if (fx + half > B_X0 + cols) fx = B_X0 + cols - half;
                }
                aafont_draw(c, F_LABEL, fx, fy, b, fc, AAFONT_CENTRE);
            }
        } else if (!is_today && dn > first) {
            for (int x = x0; x < x0 + bw; x++)
                for (int y = top; y < B_FLOOR; y++)
                    if ((x + y) % 5 == 0) pixel(c, x, y, HATCH);
        }

        /* The day's name under it; today's lit, in gold, over a small
           lozenge like the one in the Sound page's rule. */
        if (!named) continue;
        uint16_t lc = is_today ? (s->stale ? CREAM_DIM : GOLD) : CREAM_DIM;
        aafont_draw(c, F_LABEL, (int)lroundf(cx), B_LABEL_TOP, s_weekday_short[wd], lc, AAFONT_CENTRE);
        if (is_today) {
            float ly = (float)B_LABEL_TOP + (float)F_LABEL->cap + 6.0f;
            float dia[8] = { cx, ly - 2.2f, cx + 2.2f, ly, cx, ly + 2.2f, cx - 2.2f, ly };
            vec_polygon(c, dia, 4, s->stale ? CREAM_DIM : GOLD);
        }
    }
}

/* ---- the page ------------------------------------------------------------------ */

static void page(canvas_t *c, const daysui_t *s)
{
    ground(c);
    if (s == NULL) return;

    if (!s->have_data) {
        /* As the Sound page says NO SIGNAL: in the figure's place, dim, with
           whose data it is under it; no figure, no bars. */
        aafont_draw(c, F_WORD, MID, NO_DATA_TOP, "NO DATA", CREAM_DIM, AAFONT_CENTRE);
        aafont_draw(c, F_SUB, MID, NO_DATA_TOP + 40, "waiting for speaker", CREAM_DIM, AAFONT_CENTRE);
        return;
    }

    int t = today_index(s);
    const daysui_day_t *td = t >= 0 ? &s->day[t] : NULL;
    if (s->stale) tracked(c, F_LABEL, MID, CAP_TOP, "OUT OF DATE", CREAM_DIM, AAFONT_CENTRE, 2);
    else tracked(c, F_LABEL, MID, CAP_TOP, "TODAY SO FAR", GOLD, AAFONT_CENTRE, 2);
    figure(c, s, td);
    comparison(c, s, td);
    background(c, s);
    bars(c, s);
}

/* The marks blend in linear light as the type does, as on the Sound page;
   switched on for the page and put back as it was found. */
void daysui_draw(canvas_t *c, const daysui_t *s)
{
    if (c == NULL || c->fb == NULL || c->w <= 0 || c->h <= 0) return;
    bool was = vec_linear_light(true);
    page(c, s);
    vec_linear_light(was);
}
