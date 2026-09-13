#ifndef ENVPAGE_H
#define ENVPAGE_H

#include "canvas.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * One environment reading as a page: what it is now, what it did today, and
 * what it has done this month.
 *
 * The binning is separate from the drawing because the data is far larger
 * than the panel. Thirty days of one-minute samples is 43,200 readings for
 * 160 columns of chart -- 270 readings a column -- so a column has to stand
 * for a range rather than for a sample. Picking one sample per column would
 * make a cold night or a pressure crash vanish entirely depending on where
 * the sampling happened to land; a column drawn from the lowest and highest
 * of what fell in it cannot hide anything.
 *
 * Values are the fixed-point integers the log stores, not floats: the chart
 * does no arithmetic on them beyond comparison and scaling to pixels, and
 * keeping them exact means the chart and the log cannot disagree.
 */

/* One per two pixels across a 320-wide panel. */
#define ENVCHART_COLS 160

typedef struct {
    int16_t lo[ENVCHART_COLS];
    int16_t hi[ENVCHART_COLS];
    uint8_t used[(ENVCHART_COLS + 7) / 8];
    int     count;                  /* columns with anything in them */
} envchart_t;

void envchart_reset(envchart_t *c);

/* Folds one reading into its column. Out-of-range columns are ignored, so a
   caller may compute the column arithmetically without bounds-checking. */
void envchart_add(envchart_t *c, int col, int16_t value);

bool envchart_used(const envchart_t *c, int col);

/* The extremes across every column that has anything in it. */
bool envchart_range(const envchart_t *c, int16_t *lo, int16_t *hi);

/*
 * Draws the page: a title bar carrying the name and the current value, a
 * large chart of the recent window, a strip below it for the long one, and a
 * footer for the two ranges.
 *
 * Every string is the caller's: it knows the units and how many decimals each
 * reading deserves, and this knows where things go. Nothing is drawn over
 * text and no text is drawn over data.
 */
void envpage_draw(canvas_t *c, const char *title, const char *value,
                  const char *footer, uint16_t colour,
                  const envchart_t *recent, const envchart_t *longer);

#endif /* ENVPAGE_H */
