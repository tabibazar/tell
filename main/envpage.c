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
