#include "templog.h"

#include "palette.h"

#include <stdio.h>
#include <string.h>

void templog_init(templog_t *t)
{
    memset(t, 0, sizeof *t);
}

void templog_add(templog_t *t, float die, bool have_xtal, float xtal)
{
    t->die[t->head] = die;
    t->xtal[t->head] = xtal;
    t->has_xtal[t->head] = have_xtal;
    t->head = (t->head + 1) % TEMPLOG_MAX;
    if (t->n < TEMPLOG_MAX) t->n++;
}

int templog_count(const templog_t *t) { return t->n; }

/* Where sample `i` sits in the ring, counting from the oldest. Until the ring
   has wrapped the oldest is at 0; afterwards it is wherever the head points,
   because the head is about to overwrite it. */
static int slot(const templog_t *t, int i)
{
    if (t->n < TEMPLOG_MAX) return i;
    return (t->head + i) % TEMPLOG_MAX;
}

float templog_die(const templog_t *t, int i)
{
    if (i < 0 || i >= t->n) return 0.0f;
    return t->die[slot(t, i)];
}

bool templog_xtal(const templog_t *t, int i, float *out)
{
    if (i < 0 || i >= t->n) return false;
    int k = slot(t, i);
    if (!t->has_xtal[k]) return false;
    if (out) *out = t->xtal[k];
    return true;
}

bool templog_range(const templog_t *t, float *lo, float *hi)
{
    if (t->n <= 0) return false;
    float a = templog_die(t, 0), b = a;
    for (int i = 0; i < t->n; i++) {
        float v = templog_die(t, i);
        if (v < a) a = v;
        if (v > b) b = v;
        if (templog_xtal(t, i, &v)) {
            if (v < a) a = v;
            if (v > b) b = v;
        }
    }
    /* A flat trace would divide by zero when scaling, and drawing it hard
       against one edge would read as a fault rather than as steadiness. */
    if (b - a < 1.0f) { float mid = (a + b) / 2.0f; a = mid - 0.5f; b = mid + 0.5f; }
    *lo = a; *hi = b;
    return true;
}

/* Where a value sits in the plot, in pixels, clamped to it. */
static int plot_y(float v, float lo, float hi, int top, int bottom)
{
    float t = (v - lo) / (hi - lo);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    int y = bottom - (int)(t * (float)(bottom - top));
    if (y < top) y = top;
    if (y > bottom) y = bottom;
    return y;
}

/* A segment of the trace: a column from the last point to this one, so a
   steep rise is a line rather than a row of dots. */
static void column(canvas_t *c, int x, int w, int y0, int y1, uint16_t colour)
{
    int top = y0 < y1 ? y0 : y1;
    int h = (y0 < y1 ? y1 - y0 : y0 - y1) + 1;
    canvas_fill_rect(c, x, top, w, h, colour);
}

void templog_draw(const templog_t *t, canvas_t *c)
{
    canvas_clear(c);
    canvas_fill_rect(c, 0, 0, c->w, c->cell_h, PAL_TITLE_BG);

    float lo, hi;
    if (!templog_range(t, &lo, &hi)) {
        canvas_puts(c, 1, 0, "TEMP", PAL_FG);
        canvas_puts(c, 1, 2, "nothing logged yet", PAL_DIM);
        return;
    }

    char buf[40];
    float die_now = templog_die(t, t->n - 1);
    float x_now;
    bool has_x = templog_xtal(t, t->n - 1, &x_now);
    if (has_x) snprintf(buf, sizeof buf, "TEMP die%.1f xtal%.1f",
                        (double)die_now, (double)x_now);
    else       snprintf(buf, sizeof buf, "TEMP die%.1f", (double)die_now);
    canvas_puts(c, 0, 0, buf, PAL_FG);

    const int top = c->cell_h + 4;
    const int bottom = c->h - 1;
    if (bottom - top < 8) return;          /* no room to draw anything honest */

    /* The extremes, as the lines the eye reads the chart against. */
    canvas_fill_rect(c, 0, top, c->w, 1, PAL_DIM);
    canvas_fill_rect(c, 0, bottom, c->w, 1, PAL_DIM);
    snprintf(buf, sizeof buf, "%.1f", (double)hi);
    canvas_puts_px(c, c->w - (int)strlen(buf) * c->cell_w - 2, top + 1, buf, PAL_DIM);
    snprintf(buf, sizeof buf, "%.1f", (double)lo);
    canvas_puts_px(c, c->w - (int)strlen(buf) * c->cell_w - 2,
                   bottom - c->cell_h - 1, buf, PAL_DIM);

    /*
     * The die as bars first, its own line over the top. The bars give the
     * shape at a glance -- how warm, how long -- and the line gives the
     * detail the bars flatten. They are the same hue so the pair reads as
     * one series drawn twice, with the bars too dark to compete.
     *
     * One sample per two pixels, oldest at the left. Within a column the
     * order is bar, then die line, then crystal line, so the thinner thing
     * always ends up on top of the thicker one.
     */
    const int step = c->w / TEMPLOG_MAX > 0 ? c->w / TEMPLOG_MAX : 1;
    int die_hi_x = 0, die_lo_x = 0;
    float die_hi_v = templog_die(t, 0), die_lo_v = die_hi_v;

    int prev_die = 0, prev_x = 0;
    bool have_prev_x = false;
    for (int i = 0; i < t->n; i++) {
        int x = i * step;
        if (x >= c->w) break;

        float v = templog_die(t, i);
        int y = plot_y(v, lo, hi, top, bottom);
        /* The bar: from the reading down to the floor of the plot. */
        canvas_fill_rect(c, x, y, step, bottom - y + 1, pal_darken(PAL_A1));
        column(c, x, step, i == 0 ? y : prev_die, y, PAL_A1);
        prev_die = y;
        if (v > die_hi_v) { die_hi_v = v; die_hi_x = x; }
        if (v < die_lo_v) { die_lo_v = v; die_lo_x = x; }

        float xv;
        if (templog_xtal(t, i, &xv)) {
            int xy = plot_y(xv, lo, hi, top, bottom);
            column(c, x, step, have_prev_x ? prev_x : xy, xy, PAL_A0);
            prev_x = xy;
            have_prev_x = true;
        }
    }

    /* The die's own high and low, marked where they happened. The crystal
       barely moves, so marking it too would be four dots saying one thing. */
    canvas_disc(c, die_hi_x + step / 2, plot_y(die_hi_v, lo, hi, top, bottom), 3, PAL_A5);
    canvas_disc(c, die_lo_x + step / 2, plot_y(die_lo_v, lo, hi, top, bottom), 3, PAL_A2);
}
