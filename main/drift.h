#ifndef DRIFT_H
#define DRIFT_H

#include "canvas.h"

#include <stdbool.h>

/*
 * How far the board's own clock has slipped against the DS3231, charted.
 *
 * The naive version of this page would plot "drift": the chip's time less the
 * board's, in seconds. It would be a flat line at zero for a day. A DS3231 is
 * specified at +/-2 ppm, which is about a sixth of a second per day, so on any
 * timescale you would sit and watch, the chip does not move.
 *
 * What does move is the other side of the comparison. The ESP32's crystal is
 * an ordinary one, tens of ppm, so it slips tens of milliseconds an hour
 * against the chip -- and that is visible within the time it takes to make a
 * cup of tea. So the series here is PHASE: where the board's clock was, in
 * milliseconds, at the instant the chip's seconds register incremented. The
 * slope of that line is the board's error in parts per million, measured
 * against the best reference on the desk.
 *
 * Two honest limits, both of which the page says out loud:
 *
 *  - The edge is caught by polling, so each point carries the uncertainty of
 *    the poll interval. The caller reports the midpoint of the window it saw
 *    the edge in, which centres the error rather than biasing it, but a single
 *    point is still worth only tens of milliseconds. The slope is worth much
 *    more than any one point, because the fit averages the noise down.
 *
 *  - This measures the board against the chip, not the chip against the truth.
 *    Nothing here can do the latter. If the numbers say the board is 14 ppm
 *    fast, that means "fast relative to the DS3231", and the DS3231 is the one
 *    with the oven-less temperature compensation and the datasheet.
 *
 * The crystal's own temperature rides along as a second trace, because it is
 * the thing a DS3231 exists to compensate for: if the compensation is working,
 * the temperature moves and the phase slope does not.
 *
 * Like the temperature log, the ring and every pixel of the chart live here,
 * free of I2C and panels, so both can be checked on the host.
 */

/* One sample per two pixels across a 320-wide panel. */
#define DRIFT_MAX 160

typedef struct {
    int   every_s;                 /* nominal seconds between samples */
    float phase_ms[DRIFT_MAX];     /* board clock less chip clock, milliseconds */
    float xtal[DRIFT_MAX];         /* the chip's crystal temperature */
    bool  has_xtal[DRIFT_MAX];
    int   n;                       /* how many are held, up to DRIFT_MAX */
    int   head;                    /* where the next one goes */
} drift_t;

void drift_init(drift_t *d, int seconds_between_samples);

/* Adds a sample, dropping the oldest once full. `xtal` is ignored unless
   `have_xtal`, so a chip that will not report its temperature still charts
   the phase. */
void drift_add(drift_t *d, float phase_ms, bool have_xtal, float xtal);

int drift_count(const drift_t *d);

/* Oldest first, so index 0 is the left of the chart. The raw phase is the
   board's clock against the chip's, in milliseconds -- but see drift_slip_ms:
   its absolute value is an accident of how the board was set. */
float drift_phase(const drift_t *d, int i);
bool  drift_xtal(const drift_t *d, int i, float *out);

/*
 * The phase relative to the oldest sample held: how far the board has moved
 * against the chip since this record began.
 *
 * This is the number worth showing, and the raw phase is not. A board that
 * took its time from the chip did so to the nearest second, so it starts life
 * anywhere within half a second of the chip and stays there; "-377 ms" says
 * only that, and says it for ever. What moves -- what the crystal is actually
 * doing -- is the change, so the page and the chart both work in it, and the
 * chart's zero line is where the record started.
 */
float drift_rel(const drift_t *d, int i);
float drift_slip_ms(const drift_t *d);

/* The lowest and highest of one series, each scaled to itself: the phase is
   in milliseconds and the temperature in degrees, so a shared scale would be
   meaningless as well as flat. The phase range is of drift_rel, not of the
   raw phase. */
bool drift_range(const drift_t *d, bool phase_series, float *lo, float *hi);

/* How long the log covers, in seconds. */
int drift_span_s(const drift_t *d);

/*
 * The board's error in parts per million: the least-squares slope of the
 * phase against time. Least squares rather than first-to-last because every
 * point carries poll noise of its own, and a fit over a hundred of them beats
 * a line drawn through the two noisiest choices available.
 *
 * False until there are enough samples over enough time to mean anything --
 * a slope from four points ten seconds apart is a measurement of the noise.
 */
bool drift_ppm(const drift_t *d, float *ppm);

/* The same fit, as a line: milliseconds per second and the phase it implies
   at the oldest sample. The chart draws this rather than joining the points,
   because the points carry poll noise many times larger than the movement
   between any two of them -- joined up they are a comb, and the trend, which
   is the thing being measured, disappears into it. */
bool drift_fit(const drift_t *d, float *slope_ms_per_s, float *intercept_ms);

/*
 * Draws the chart from `top_row` to the bottom of the panel: the samples as a
 * scatter, the fitted trend over them once it means anything, the crystal
 * temperature as a second scatter scaled to itself, and a dotted line at
 * where the record started.
 *
 * The samples are spread across the full width rather than laid down one per
 * two pixels from the left. Filling from the left is right for a log you
 * leave running; this is a page you turn to, and forty minutes of waiting to
 * see a graph rather than a stub in the corner is not a trade worth making.
 * The span in the caller's header says what the width covers.
 *
 * Does not clear; the caller owns the rows above.
 */
void drift_draw(const drift_t *d, canvas_t *c, int top_row);

#endif /* DRIFT_H */
