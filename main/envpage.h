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
 * Writes one axis label for a raw value. The caller owns it because the
 * caller knows the units and how many decimals each reading deserves; a
 * pressure wants none where a temperature wants one, and an axis of "984.7,
 * 985.2, 985.7" on a panel this narrow is unreadable where "985, 990" is not.
 */
typedef void (*envfmt_fn)(int16_t raw, char *out, int size);

/* Columns reserved down the left for the value axis. Four is enough for
   "24.5" or "1013" and costs an eighth of the width. */
#define ENVPAGE_GUTTER 4

/*
 * Draws the page: a title bar with the name and the current value, a charted
 * window with both axes, the hours beneath it, and the long window as a strip
 * along the bottom.
 *
 * The axes are the point of this rather than decoration. Without them a trace
 * says only "it went up a bit", because every chart here is scaled to its own
 * range and a given height means nothing absolute. The value axis is drawn in
 * its own gutter so no label sits over the data, and the gridlines go down
 * before the trace so the trace wins wherever they meet.
 *
 * `end_minute` is the minute of the day at the right-hand edge, so the hour
 * marks can be real clock times rather than "twelve hours ago"; pass -1 when
 * the time is unknown and the hours are left off.
 */
/* A reference line drawn across the chart, at a raw value, with a short label
   -- "ok", "vent" -- so a reading means something without knowing the numbers. */
typedef struct {
    int16_t     value;
    const char *label;
} env_thresh_t;

typedef struct {
    const char *title;
    const char *value;
    uint16_t    colour;
    envfmt_fn   fmt;
    const envchart_t *recent;     /* the big chart */
    const envchart_t *longer;     /* the strip along the bottom */
    int span_minutes;             /* what `recent` covers, for the hour marks */
    int end_minute;               /* minute of the day at the right edge, or -1 */
    const env_thresh_t *thresh;   /* reference lines drawn across, or NULL */
    int thresh_n;
} envpage_t;

void envpage_draw(canvas_t *c, const envpage_t *p);

/*
 * A round step for an axis: 1, 2 or 5 times a power of ten, the largest that
 * still puts at least two lines inside the range. Round numbers are the whole
 * reason an axis is readable -- gridlines at 23.7 and 24.4 are arithmetic
 * nobody does at a glance.
 */
int envchart_nice_step(int16_t lo, int16_t hi, int max_lines);

/*
 * Two readings over the same window, on one chart.
 *
 * Each is scaled to its own range, because a temperature in hundredths of a
 * degree and a humidity in hundredths of a per cent share no scale worth
 * having. Height therefore means "where in its own range", and the footer
 * carries both ranges -- which is the price of putting them together, and
 * worth paying: what the page is for is the shape of one against the other,
 * and that survives the separate scaling intact.
 */
/* Two readings stacked -- `a` in the top half of the panel, `b` in the bottom,
   each a titled chart with its own axis. Fills a tall portrait panel that one
   chart leaves half empty. */
void env2_draw(canvas_t *c, const envpage_t *a, const envpage_t *b);

void envpair_draw(canvas_t *c, const char *title, const char *value,
                  const char *footer, const envchart_t *a, uint16_t colour_a,
                  const envchart_t *b, uint16_t colour_b);

/*
 * The week: each day's low and high as a bar, seven of them side by side.
 *
 * A different question from the charts above, and it wants a different shape.
 * The charts answer "what is it doing"; this answers "was yesterday colder
 * than today", which is a comparison between seven things and so wants seven
 * things you can put a ruler against. The bars share one scale for that
 * reason -- unlike every other chart here, where each trace is scaled to
 * itself, a week whose days were each scaled separately would make every day
 * look identical.
 */
#define ENVWEEK_DAYS 7

typedef struct {
    int16_t lo[ENVWEEK_DAYS];
    int16_t hi[ENVWEEK_DAYS];
    uint8_t used;                    /* one bit per day */
    char    label[ENVWEEK_DAYS];     /* the day's initial, or ' ' */
} envweek_t;

void envweek_reset(envweek_t *w);

/* Day 0 is the oldest of the seven, day 6 is today. */
void envweek_add(envweek_t *w, int day, int16_t value);
void envweek_label(envweek_t *w, int day, char initial);
bool envweek_used(const envweek_t *w, int day);
bool envweek_range(const envweek_t *w, int16_t *lo, int16_t *hi);

void envweek_draw(canvas_t *c, const char *title, const char *value,
                  uint16_t colour, const envweek_t *w);

#endif /* ENVPAGE_H */
