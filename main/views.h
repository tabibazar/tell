#ifndef VIEWS_H
#define VIEWS_H

#include "canvas.h"
#include "usagedata.h"

/* Each fills the canvas; the caller blits. */
void views_stats(canvas_t *c, const usagedata_t *d);
void views_daily(canvas_t *c, const usagedata_t *d);

#endif /* VIEWS_H */
