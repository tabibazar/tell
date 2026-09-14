#include "envpage.h"

#include "palette.h"

#include <stdio.h>
#include <string.h>

void envchart_reset(envchart_t *c)
{
    memset(c, 0, sizeof *c);
}

bool envchart_used(const envchart_t *c, int col)
{
    if (col < 0 || col >= ENVCHART_COLS) return false;
    return (c->used[col / 8] & (1u << (col % 8))) != 0;
}

void envchart_add(envchart_t *c, int col, int16_t value)
{
    if (col < 0 || col >= ENVCHART_COLS) return;
    if (!envchart_used(c, col)) {
        c->used[col / 8] |= (uint8_t)(1u << (col % 8));
        c->lo[col] = c->hi[col] = value;
        c->count++;
        return;
    }
    if (value < c->lo[col]) c->lo[col] = value;
    if (value > c->hi[col]) c->hi[col] = value;
}

bool envchart_range(const envchart_t *c, int16_t *lo, int16_t *hi)
{
    bool any = false;
    int16_t a = 0, b = 0;
    for (int i = 0; i < ENVCHART_COLS; i++) {
        if (!envchart_used(c, i)) continue;
        if (!any) { a = c->lo[i]; b = c->hi[i]; any = true; continue; }
        if (c->lo[i] < a) a = c->lo[i];
        if (c->hi[i] > b) b = c->hi[i];
    }
    if (!any) return false;
    *lo = a; *hi = b;
    return true;
}

/* Where a value sits between top and bottom, clamped. */
static int plot_y(int16_t v, int16_t lo, int16_t hi, int top, int bottom)
{
    if (hi <= lo) return (top + bottom) / 2;
    long span = (long)hi - (long)lo;
    long up = ((long)v - (long)lo) * (long)(bottom - top) / span;
    int y = bottom - (int)up;
    if (y < top) y = top;
    if (y > bottom) y = bottom;
    return y;
}

/*
 * A chart as a band per column: the lowest to the highest of everything that
 * fell in it. A steady reading draws a thin line, a swinging one draws a thick
 * band, and neither can conceal the other.
 */
static void band_at(canvas_t *c, const envchart_t *ch, int left, int top,
                    int bottom, uint16_t colour)
{
    if (bottom - top < 2) return;
    if (left < 0) left = 0;
    if (left >= c->w) return;
    int16_t lo, hi;
    if (!envchart_range(ch, &lo, &hi)) return;

    /* A flat trace scaled to itself would divide by zero, and drawn hard
       against an edge it reads as a fault rather than as steadiness. */
    /*
     * Each column spans from its own edge to the next one's, worked out from
     * the width rather than from a fixed integer step. With a gutter the plot
     * is 272 pixels and there are 160 columns, so an integer step is 1 and
     * would draw the whole chart into the left 160 pixels, leaving a third of
     * the panel blank and the time axis beneath it lying about where the data
     * is.
     */
    int width = c->w - left;

    if (hi == lo) {
        int mid = (top + bottom) / 2;
        for (int i = 0; i < ENVCHART_COLS; i++) {
            if (!envchart_used(ch, i)) continue;
            int x0 = left + (int)((long)i * width / ENVCHART_COLS);
            int x1 = left + (int)((long)(i + 1) * width / ENVCHART_COLS);
            canvas_fill_rect(c, x0, mid, x1 > x0 ? x1 - x0 : 1, 1, colour);
        }
        return;
    }

    for (int i = 0; i < ENVCHART_COLS; i++) {
        if (!envchart_used(ch, i)) continue;
        int x0 = left + (int)((long)i * width / ENVCHART_COLS);
        int x1 = left + (int)((long)(i + 1) * width / ENVCHART_COLS);
        int y0 = plot_y(ch->hi[i], lo, hi, top, bottom);
        int y1 = plot_y(ch->lo[i], lo, hi, top, bottom);
        canvas_fill_rect(c, x0, y0, x1 > x0 ? x1 - x0 : 1, y1 - y0 + 1, colour);
    }
}

/* The pair page and the week draw edge to edge, with no gutter. */
static void band(canvas_t *c, const envchart_t *ch, int top, int bottom,
                 uint16_t colour)
{
    band_at(c, ch, 0, top, bottom, colour);
}

int envchart_nice_step(int16_t lo, int16_t hi, int max_lines)
{
    int span = hi - lo;
    if (span <= 0 || max_lines < 1) return 0;

    /* 1, 2, 5, 10, 20, 50 ... the first that does not put more than
       max_lines gridlines inside the span. Kept as a mantissa and a power of
       ten rather than derived from the previous step: working it out from the
       last value gets 20 -> 40 instead of 20 -> 50, which is a step nobody
       reads at a glance and exactly the thing round numbers are for. */
    int mant = 1, pow10 = 1;
    for (int guard = 0; guard < 40; guard++) {
        int step = mant * pow10;
        if (span / step <= max_lines) return step;
        if (mant == 1) mant = 2;
        else if (mant == 2) mant = 5;
        else { mant = 1; pow10 *= 10; }
        if (pow10 > 100000) break;
    }
    return mant * pow10;
}

/* A dotted horizontal rule, so it cannot be mistaken for a trace. */
static void rule_h(canvas_t *c, int x0, int x1, int y, uint16_t colour)
{
    for (int x = x0; x < x1; x += 4) canvas_fill_rect(c, x, y, 2, 1, colour);
}

static void rule_v(canvas_t *c, int x, int y0, int y1, uint16_t colour)
{
    for (int y = y0; y < y1; y += 4) canvas_fill_rect(c, x, y, 1, 2, colour);
}

/*
 * The value axis: round gridlines across the plot, each labelled in the
 * gutter. Drawn before the trace, so where they meet the data wins.
 */
static void axis_values(canvas_t *c, const envpage_t *p, int16_t lo, int16_t hi,
                        int left, int top, int bottom, int last_row)
{
    if (p->fmt == NULL) return;
    int step = envchart_nice_step(lo, hi, 3);
    if (step <= 0) return;

    /* Start at the first round value at or above the bottom of the range. */
    int first = (lo >= 0) ? ((lo + step - 1) / step) * step
                          : -(((-lo) / step) * step);
    for (int v = first; v <= hi; v += step) {
        int y = plot_y((int16_t)v, lo, hi, top, bottom);
        rule_h(c, left, c->w, y, pal_darken(PAL_DIM));

        char label[12];
        p->fmt((int16_t)v, label, sizeof label);
        int len = (int)strlen(label);
        if (len > ENVPAGE_GUTTER) len = ENVPAGE_GUTTER;
        /* Right-aligned against the plot, and nudged so the text sits level
           with its line rather than hanging below it. */
        /* Level with its own line, and never down on the row of hours: a
           value sitting among the clock times reads as one of them. */
        int row = (y - c->cell_h / 2) / c->cell_h;
        if (row < 1) row = 1;
        if (row > last_row) row = last_row;
        canvas_puts(c, ENVPAGE_GUTTER - len, row, label, PAL_DIM);
    }
}

/*
 * The time axis: a mark every six hours, labelled with the hour of the day
 * underneath. Six hours is the coarsest spacing that still says which part of
 * the day you are looking at, and the finest that fits four labels across a
 * panel this narrow.
 */
static void axis_hours(canvas_t *c, const envpage_t *p, int left, int top,
                       int bottom, int label_row)
{
    if (p->end_minute < 0 || p->span_minutes <= 0) return;
    const int every = 6 * 60;
    int width = c->w - left;
    if (width <= 0) return;

    /* Walk back from the right edge to each earlier six-hour boundary. */
    int back = p->end_minute % every;
    for (; back < p->span_minutes; back += every) {
        int x = left + (int)((long)(p->span_minutes - back) * width
                             / p->span_minutes) - 1;
        if (x < left || x >= c->w) continue;
        rule_v(c, x, top, bottom, pal_darken(PAL_DIM));

        int minute = ((p->end_minute - back) % 1440 + 1440) % 1440;
        char label[4];
        snprintf(label, sizeof label, "%02d", minute / 60);
        int col = (x - c->cell_w) / c->cell_w;
        if (col < ENVPAGE_GUTTER) continue;      /* would sit in the gutter */
        if (col > c->cols - 2) continue;
        canvas_puts(c, col, label_row, label, PAL_DIM);
    }
}

void envpage_draw(canvas_t *c, const envpage_t *p)
{
    canvas_clear(c);
    canvas_fill_rect(c, 0, 0, c->w, c->cell_h, PAL_TITLE_BG);
    if (p->title) canvas_puts(c, 0, 0, p->title, PAL_FG);
    if (p->value) {
        int len = (int)strlen(p->value);
        canvas_puts(c, c->cols - len, 0, p->value, PAL_FG);
    }
    if (c->rows < 5) return;          /* nothing honest to draw */

    /*
     * Rows one to four are the window being watched, row five carries the
     * hours that belong to it, and row six is the long strip. The hours sit
     * directly under their own chart rather than under the strip, which would
     * read as labelling the wrong thing.
     */
    const int left = ENVPAGE_GUTTER * c->cell_w;
    const int top = c->cell_h + 1;
    const int bottom = 5 * c->cell_h - 2;
    const int hour_row = c->rows - 2;
    const int strip_top = (c->rows - 1) * c->cell_h + 1;
    const int strip_bottom = c->h - 1;

    int16_t lo, hi;
    if (envchart_range(p->recent, &lo, &hi)) {
        axis_values(c, p, lo, hi, left, top, bottom, hour_row - 1);
        axis_hours(c, p, left, top, bottom, hour_row);
    }
    band_at(c, p->recent, left, top, bottom, p->colour);

    /* The strip, in the darker shade, with no axis of its own: it is there to
       say where today sits in the month, and a second set of labels on four
       rows of chart would cost more than it told. */
    band_at(c, p->longer, left, strip_top, strip_bottom, pal_darken(p->colour));
    canvas_fill_rect(c, 0, (c->rows - 1) * c->cell_h - 1, c->w, 1, PAL_DIM);
}

void envpair_draw(canvas_t *c, const char *title, const char *value,
                  const char *footer, const envchart_t *a, uint16_t colour_a,
                  const envchart_t *b, uint16_t colour_b)
{
    canvas_clear(c);
    canvas_fill_rect(c, 0, 0, c->w, c->cell_h, PAL_TITLE_BG);
    if (title) canvas_puts(c, 0, 0, title, PAL_FG);
    if (value) {
        int len = (int)strlen(value);
        canvas_puts(c, c->cols - len, 0, value, PAL_FG);
    }
    if (c->rows < 3) return;

    /* Everything between the title and the footer belongs to both traces. */
    const int top = c->cell_h + 1;
    const int bottom = (c->rows - 1) * c->cell_h - 2;
    if (bottom - top < 6) return;

    band(c, a, top, bottom, colour_a);
    band(c, b, top, bottom, colour_b);

    if (footer) canvas_puts(c, 0, c->rows - 1, footer, PAL_DIM);
}

void envweek_reset(envweek_t *w)
{
    memset(w, 0, sizeof *w);
    for (int i = 0; i < ENVWEEK_DAYS; i++) w->label[i] = ' ';
}

bool envweek_used(const envweek_t *w, int day)
{
    if (day < 0 || day >= ENVWEEK_DAYS) return false;
    return (w->used & (1u << day)) != 0;
}

void envweek_add(envweek_t *w, int day, int16_t value)
{
    if (day < 0 || day >= ENVWEEK_DAYS) return;
    if (!envweek_used(w, day)) {
        w->used |= (uint8_t)(1u << day);
        w->lo[day] = w->hi[day] = value;
        return;
    }
    if (value < w->lo[day]) w->lo[day] = value;
    if (value > w->hi[day]) w->hi[day] = value;
}

void envweek_label(envweek_t *w, int day, char initial)
{
    if (day < 0 || day >= ENVWEEK_DAYS) return;
    w->label[day] = initial;
}

bool envweek_range(const envweek_t *w, int16_t *lo, int16_t *hi)
{
    bool any = false;
    int16_t a = 0, b = 0;
    for (int i = 0; i < ENVWEEK_DAYS; i++) {
        if (!envweek_used(w, i)) continue;
        if (!any) { a = w->lo[i]; b = w->hi[i]; any = true; continue; }
        if (w->lo[i] < a) a = w->lo[i];
        if (w->hi[i] > b) b = w->hi[i];
    }
    if (!any) return false;
    *lo = a; *hi = b;
    return true;
}

void envweek_draw(canvas_t *c, const char *title, const char *value,
                  uint16_t colour, const envweek_t *w)
{
    canvas_clear(c);
    canvas_fill_rect(c, 0, 0, c->w, c->cell_h, PAL_TITLE_BG);
    if (title) canvas_puts(c, 0, 0, title, PAL_FG);
    if (value) {
        int len = (int)strlen(value);
        canvas_puts(c, c->cols - len, 0, value, PAL_FG);
    }
    if (c->rows < 3) return;

    int16_t lo, hi;
    if (!envweek_range(w, &lo, &hi)) {
        canvas_puts(c, 1, 2, "no days logged yet", PAL_DIM);
        return;
    }

    /* The bars live between the title and the row of day initials. */
    const int top = c->cell_h + 2;
    const int bottom = (c->rows - 1) * c->cell_h - 3;
    if (bottom - top < 6) return;

    /* A day whose low and high are the same -- a board switched on an hour
       ago -- would otherwise be an invisible zero-height bar. */
    if (hi == lo) { lo = (int16_t)(lo - 1); hi = (int16_t)(hi + 1); }

    const int slot = c->w / ENVWEEK_DAYS;
    const int bar = slot * 2 / 3;
    const int pad = (slot - bar) / 2;

    for (int i = 0; i < ENVWEEK_DAYS; i++) {
        int x = i * slot + pad;
        if (envweek_used(w, i)) {
            int y0 = plot_y(w->hi[i], lo, hi, top, bottom);
            int y1 = plot_y(w->lo[i], lo, hi, top, bottom);
            canvas_fill_rect(c, x, y0, bar, y1 - y0 + 1, colour);
        } else {
            /* A day with no readings is a gap, drawn as a dotted floor so it
               reads as "nothing recorded" rather than as "zero". */
            for (int k = 0; k < bar; k += 4)
                canvas_fill_rect(c, x + k, bottom, 2, 1, PAL_DIM);
        }
        /* The initial, centred under its bar, on its own row: no text over
           data and no data over text. */
        if (w->label[i] != ' ') {
            char s[2] = { w->label[i], '\0' };
            int col = (x + bar / 2) / c->cell_w;
            canvas_puts(c, col, c->rows - 1, s, PAL_DIM);
        }
    }
}
