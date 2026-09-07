#ifndef VIEWS_H
#define VIEWS_H

#include "canvas.h"
#include "usagedata.h"

/* Each fills the canvas; the caller blits. Both draw the merged view, so
   several machines' contributions appear as one set of totals.

   `t` runs 0..1 and scales the bars, so a page can grow into place when it
   appears. Pass 1 for the finished chart. */
void views_stats(canvas_t *c, const ud_view_t *d, float t);
void views_daily(canvas_t *c, const ud_view_t *d, float t);

/* The almanac page: moon, sun, the day's numbers and the next holiday. */
void views_today(canvas_t *c, const usagedata_t *d);

#endif /* VIEWS_H */
