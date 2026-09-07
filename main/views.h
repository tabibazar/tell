#ifndef VIEWS_H
#define VIEWS_H

#include "canvas.h"
#include "settings.h"
#include "usagedata.h"

#include <stdbool.h>

/* Each fills the canvas; the caller blits. Both draw the merged view, so
   several machines' contributions appear as one set of totals.

   `t` runs 0..1 and scales the bars, so a page can grow into place when it
   appears. Pass 1 for the finished chart. */
void views_stats(canvas_t *c, const ud_view_t *d, float t, int64_t now_us);
void views_daily(canvas_t *c, const ud_view_t *d, float t, int64_t now_us);

/* Tokens per day as a step line per model, the biggest models as cards
   beneath. `t` grows the lines like the bars above. */
void views_models(canvas_t *c, const ud_view_t *d, float t, int64_t now_us);

/* The last twelve months as a heatmap, with the headline figures. */
void views_year(canvas_t *c, const ud_view_t *d, int64_t now_us);

/* What the usage would have cost on the API: totals, a bar per model, and
   a bar per day for the last two months. `t` grows the bars. */
void views_cost(canvas_t *c, const ud_view_t *d, float t, int64_t now_us);

/* When you work: messages by weekday and hour as a heatmap. */
void views_rhythm(canvas_t *c, const ud_view_t *d, int64_t now_us);

/* Today, live: this day's figures and whether Claude is busy right now.
   Redraw it every few seconds; the "last message" age keeps counting. */
void views_now(canvas_t *c, const ud_view_t *d, int64_t now_us);

/* The settings page: each setting as a row of buttons, the current choice
   lit. Touch only, so it is not offered on the Feather. */
void views_settings(canvas_t *c, const settings_t *s);

/* True when a tap at display pixel (x, y) landed on a settings button, with
   which one. Buttons are hit-tested taller than they are drawn: a text row
   is 4mm on this panel, smaller than a fingertip. */
bool views_settings_hit(canvas_t *c, int x, int y, int *row, int *choice);

/* The almanac page: moon, sun, the day's numbers and the next holiday. */
void views_today(canvas_t *c, const usagedata_t *d);

#endif /* VIEWS_H */
