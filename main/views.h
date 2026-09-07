#ifndef VIEWS_H
#define VIEWS_H

#include "canvas.h"
#include "usagedata.h"

/* Each fills the canvas; the caller blits. Both draw the merged view, so
   several machines' contributions appear as one set of totals. */
void views_stats(canvas_t *c, const ud_view_t *d);
void views_daily(canvas_t *c, const ud_view_t *d);

#endif /* VIEWS_H */
