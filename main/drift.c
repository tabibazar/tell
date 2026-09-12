#include "drift.h"

#include "palette.h"

#include <math.h>
#include <string.h>

/* Below this the slope is a measurement of the poll noise rather than of the
   crystal: a handful of points a few seconds apart can "prove" any ppm you
   like. Ten minutes of samples is where the fit starts to mean something. */
#define DRIFT_MIN_SAMPLES 8
#define DRIFT_MIN_SPAN_S  300

/* How well the slope must be known before it is worth printing. A few ppm is
   about what a fit over this sample rate reaches in twenty minutes, and it is
   also about the precision the figure is quoted to. */
#define DRIFT_MAX_SE_PPM  3.0f

void drift_init(drift_t *d, int seconds_between_samples)
{
    memset(d, 0, sizeof *d);
    d->every_s = seconds_between_samples > 0 ? seconds_between_samples : 1;
}

void drift_add(drift_t *d, float phase_ms, bool have_xtal, float xtal)
{
    d->phase_ms[d->head] = phase_ms;
    d->xtal[d->head] = xtal;
    d->has_xtal[d->head] = have_xtal;
    d->head = (d->head + 1) % DRIFT_MAX;
    if (d->n < DRIFT_MAX) d->n++;
}

int drift_count(const drift_t *d) { return d->n; }

int drift_span_s(const drift_t *d)
{
    return d->n > 1 ? (d->n - 1) * d->every_s : 0;
}

/* Where sample `i` sits in the ring, counting from the oldest. Until the ring
   has wrapped the oldest is at 0; afterwards it is wherever the head points,
   because the head is about to overwrite it. */
static int slot(const drift_t *d, int i)
{
    if (d->n < DRIFT_MAX) return i;
    return (d->head + i) % DRIFT_MAX;
}

float drift_phase(const drift_t *d, int i)
{
    if (i < 0 || i >= d->n) return 0.0f;
    return d->phase_ms[slot(d, i)];
}

bool drift_xtal(const drift_t *d, int i, float *out)
{
    if (i < 0 || i >= d->n) return false;
    int s = slot(d, i);
    if (!d->has_xtal[s]) return false;
    *out = d->xtal[s];
    return true;
}

float drift_rel(const drift_t *d, int i)
{
    if (d->n <= 0) return 0.0f;
    return drift_phase(d, i) - drift_phase(d, 0);
}

float drift_slip_ms(const drift_t *d)
{
    return d->n > 0 ? drift_rel(d, d->n - 1) : 0.0f;
}

bool drift_range(const drift_t *d, bool phase_series, float *lo, float *hi)
{
    if (d->n <= 0) return false;
    float a = 0.0f, b = 0.0f;
    bool any = false;
    for (int i = 0; i < d->n; i++) {
        float v;
        if (phase_series) v = drift_rel(d, i);
        else if (!drift_xtal(d, i, &v)) continue;
        if (!any) { a = b = v; any = true; continue; }
        if (v < a) a = v;
        if (v > b) b = v;
    }
    if (!any) return false;

    /* A flat trace would divide by zero when scaled, and drawn hard against
       one edge it would read as a fault rather than as steadiness. The floor
       differs by series: a millisecond of phase and a tenth of a degree are
       each about the smallest real movement of their sensor. */
    /* The DS3231 reports temperature in quarter degrees. Scaled to a range
       narrower than about a degree, that quantisation becomes a staircase
       across the whole panel -- six discrete heights pretending to be a
       signal. A degree is the smallest range worth spreading out. */
    float floor_span = phase_series ? 2.0f : 1.0f;
    if (b - a < floor_span) {
        float mid = (a + b) / 2.0f;
        a = mid - floor_span / 2.0f;
        b = mid + floor_span / 2.0f;
    }
    *lo = a; *hi = b;
    return true;
}

bool drift_fit(const drift_t *d, float *slope_ms_per_s, float *intercept_ms)
{
    if (d->n < 2) return false;

    /*
     * Least squares on (seconds, milliseconds). x is the sample index times
     * the interval, so the slope comes out in milliseconds per second, and a
     * millisecond per second is a thousand parts per million.
     */
    double n = (double)d->n;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < d->n; i++) {
        double x = (double)i * (double)d->every_s;
        double y = (double)drift_phase(d, i);
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    double denom = n * sxx - sx * sx;
    if (denom == 0.0) return false;
    double slope = (n * sxy - sx * sy) / denom;
    if (slope_ms_per_s) *slope_ms_per_s = (float)slope;
    if (intercept_ms) *intercept_ms = (float)((sy - slope * sx) / n);
    return true;
}

bool drift_ppm_err(const drift_t *d, float *ppm, float *se_ppm)
{
    /* Enough points over enough time even to attempt it: a handful of samples
       a few seconds apart can "prove" any ppm you like. */
    if (d->n < DRIFT_MIN_SAMPLES) return false;
    if (drift_span_s(d) < DRIFT_MIN_SPAN_S) return false;

    float slope, intercept;
    if (!drift_fit(d, &slope, &intercept)) return false;
    if (d->n < 3) return false;                  /* no residual degrees of freedom */

    /* The residual scatter about the fit, and from it how well the slope is
       known: the usual standard error of a least-squares gradient. */
    double ss_res = 0.0, sxx = 0.0, mean_x = 0.0;
    for (int i = 0; i < d->n; i++) mean_x += (double)i * (double)d->every_s;
    mean_x /= (double)d->n;
    for (int i = 0; i < d->n; i++) {
        double x = (double)i * (double)d->every_s;
        double r = (double)drift_phase(d, i) - ((double)intercept + (double)slope * x);
        ss_res += r * r;
        sxx += (x - mean_x) * (x - mean_x);
    }
    if (sxx <= 0.0) return false;
    double se = sqrt((ss_res / (double)(d->n - 2)) / sxx) * 1000.0;

    if (ppm) *ppm = slope * 1000.0f;
    if (se_ppm) *se_ppm = (float)se;
    return se <= (double)DRIFT_MAX_SE_PPM;
}

bool drift_ppm(const drift_t *d, float *ppm)
{
    return drift_ppm_err(d, ppm, NULL);
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

/* A vertical run joining one sample's height to the next, so a line drawn one
   sample per column is continuous rather than a row of dots. */
static void column(canvas_t *c, int x, int w, int y0, int y1, uint16_t colour)
{
    int top = y0 < y1 ? y0 : y1;
    int h = (y0 < y1 ? y1 - y0 : y0 - y1) + 1;
    canvas_fill_rect(c, x, top, w, h, colour);
}

/* Where sample `i` sits across the panel, spread over the full width. */
static int plot_x(const drift_t *d, int i, int w, int dot)
{
    if (d->n <= 1) return 0;
    long span = (long)w - dot;
    return (int)((long)i * span / (long)(d->n - 1));
}

void drift_draw(const drift_t *d, canvas_t *c, int top_row)
{
    const int top = top_row * c->cell_h;
    const int bottom = c->h - 1;
    if (bottom - top < 8) return;        /* no room to draw anything honest */

    float plo, phi;
    if (!drift_range(d, true, &plo, &phi)) return;
    float xlo = 0.0f, xhi = 0.0f;
    bool has_x = drift_range(d, false, &xlo, &xhi);

    /*
     * Where the record started, dotted, underneath everything. Working in the
     * change rather than the raw phase makes this line always present and
     * always meaningful: above it the board has gained on the chip, below it
     * the board has lost. Dotted so it cannot be mistaken for data.
     */
    {
        int zy = plot_y(0.0f, plo, phi, top, bottom);
        for (int x = 0; x < c->w; x += 4)
            canvas_fill_rect(c, x, zy, 2, 1, PAL_DIM);
    }

    /* Fat enough to see on a small panel, and it also sets how much room the
       spread has to leave at the right edge. */
    const int dot = 2;

    /*
     * Whether the trend will be drawn decides how loud the samples are. With
     * a line over them they are its evidence and want to sit behind it; with
     * no line they are the entire chart, and dimming them leaves a page that
     * looks empty rather than young.
     */
    float ppm, slope, intercept;
    bool has_fit = drift_ppm(d, &ppm) && drift_fit(d, &slope, &intercept);
    uint16_t dot_colour = has_fit ? pal_darken(PAL_A0) : PAL_A0;

    /*
     * The samples as a scatter, not as a joined line. Each is worth only the
     * poll interval -- tens of milliseconds -- while the movement between two
     * of them is a fraction of that, so joining them draws the noise at full
     * height and buries the trend in it. As a scatter the noise looks like
     * what it is: a band with a slope through it.
     *
     * The temperature goes down first and dimmer. It is context -- the thing
     * a DS3231 exists to compensate for -- not the subject.
     */
    for (int i = 0; i < d->n; i++) {
        float xv;
        if (has_x && drift_xtal(d, i, &xv)) {
            int y = plot_y(xv, xlo, xhi, top, bottom);
            canvas_fill_rect(c, plot_x(d, i, c->w, dot), y, dot, 1, pal_darken(PAL_A1));
        }
    }
    for (int i = 0; i < d->n; i++) {
        int y = plot_y(drift_rel(d, i), plo, phi, top, bottom);
        canvas_fill_rect(c, plot_x(d, i, c->w, dot), y, dot, 2, dot_colour);
    }

    /*
     * And the fit over the top -- but only once it means something. Drawn
     * from a handful of noisy points it is a line through the jitter, and
     * drawing it beside a figure that honestly says "settling" would be the
     * chart contradicting its own caption. Same gate as the figure, and the
     * same arithmetic, so the two cannot disagree.
     */
    if (!has_fit) return;
    float base = drift_phase(d, 0);
    int prev = 0;
    for (int i = 0; i < d->n; i++) {
        float v = intercept + slope * (float)i * (float)d->every_s - base;
        int y = plot_y(v, plo, phi, top, bottom);
        int x = plot_x(d, i, c->w, dot);
        column(c, x, dot, i == 0 ? y : prev, y, PAL_A0);
        prev = y;
    }
}
