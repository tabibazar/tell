#include "envpage.h"

#include "palette.h"

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
static void band(canvas_t *c, const envchart_t *ch, int top, int bottom,
                 uint16_t colour)
{
    if (bottom - top < 2) return;
    int16_t lo, hi;
    if (!envchart_range(ch, &lo, &hi)) return;

    /* A flat trace scaled to itself would divide by zero, and drawn hard
       against an edge it reads as a fault rather than as steadiness. */
    if (hi == lo) {
        int mid = (top + bottom) / 2;
        for (int i = 0; i < ENVCHART_COLS; i++)
            if (envchart_used(ch, i))
                canvas_fill_rect(c, i * (c->w / ENVCHART_COLS), mid,
                                 c->w / ENVCHART_COLS, 1, colour);
        return;
    }

    int step = c->w / ENVCHART_COLS;
    if (step < 1) step = 1;
    for (int i = 0; i < ENVCHART_COLS; i++) {
        if (!envchart_used(ch, i)) continue;
        int y0 = plot_y(ch->hi[i], lo, hi, top, bottom);
        int y1 = plot_y(ch->lo[i], lo, hi, top, bottom);
        canvas_fill_rect(c, i * step, y0, step, y1 - y0 + 1, colour);
    }
}

void envpage_draw(canvas_t *c, const char *title, const char *value,
                  const char *footer, uint16_t colour,
                  const envchart_t *recent, const envchart_t *longer)
{
    canvas_clear(c);
    canvas_fill_rect(c, 0, 0, c->w, c->cell_h, PAL_TITLE_BG);
    if (title) canvas_puts(c, 0, 0, title, PAL_FG);
    if (value) {
        int len = (int)strlen(value);
        canvas_puts(c, c->cols - len, 0, value, PAL_FG);
    }

    if (c->rows < 4) return;          /* nothing honest to draw */

    /*
     * Rows one to four are today, row five is the month, row six is the two
     * ranges. The month gets a quarter of the height the day does: it is
     * there to say "and this is where today sits in the month", which a strip
     * does, and giving it equal room would halve the resolution of the chart
     * anyone actually watches.
     */
    const int recent_top = c->cell_h + 1;
    const int recent_bottom = 5 * c->cell_h - 2;
    const int long_top = 5 * c->cell_h + 1;
    const int long_bottom = 6 * c->cell_h - 2;

    band(c, recent, recent_top, recent_bottom, colour);
    band(c, longer, long_top, long_bottom, pal_darken(colour));

    /* A hairline between the two, so the strip is legible as its own chart
       rather than as the bottom of the big one. */
    canvas_fill_rect(c, 0, 5 * c->cell_h - 1, c->w, 1, PAL_DIM);

    if (footer && c->rows >= 7) canvas_puts(c, 0, 6, footer, PAL_DIM);
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
