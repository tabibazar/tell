#ifndef VIEWS_H
#define VIEWS_H

#include "canvas.h"
#include "pages.h"
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
void views_year(canvas_t *c, const ud_view_t *d, float t, int64_t now_us);

/* What the usage would have cost on the API: totals, a bar per model, and
   a bar per day for the last two months. `t` grows the bars. */
void views_cost(canvas_t *c, const ud_view_t *d, float t, int64_t now_us);

/* When you work: messages by weekday and hour as a heatmap. */
void views_rhythm(canvas_t *c, const ud_view_t *d, float t, int64_t now_us);

/* Today, live: this day's figures and whether Claude is busy right now.
   Redraw it every few seconds; the "last message" age keeps counting. */
void views_now(canvas_t *c, const ud_view_t *d, float t, int64_t now_us);

/* By project: a bar per repository with its tokens and cost. */
void views_projects(canvas_t *c, const ud_view_t *d, float t, int64_t now_us);

/* Cache efficiency: hit rate per model and per day, and what caching saved. */
void views_cache(canvas_t *c, const ud_view_t *d, float t, int64_t now_us);

/* Tools: a bar per tool Claude called, and calls per day. */
void views_tools(canvas_t *c, const ud_view_t *d, float t, int64_t now_us);

/* Thinking share: how much of each model's output was thinking. */
void views_thinking(canvas_t *c, const ud_view_t *d, float t, int64_t now_us);

/* The account's limits: a bar and a live countdown for each window. */
void views_limits(canvas_t *c, const ud_view_t *d, float t, int64_t now_us);

/* Records: personal bests, each with the day it happened. */
void views_records(canvas_t *c, const ud_view_t *d, float t, int64_t now_us);

/* What Claude runs: the programs behind the Bash calls, and how they split
   into git, build, test, files, scripts and other. */
void views_runs(canvas_t *c, const ud_view_t *d, float t, int64_t now_us);

/* Turns: how long Claude takes to reply and to finish, with a daily median. */
void views_turns(canvas_t *c, const ud_view_t *d, float t, int64_t now_us);

/* The menu: a tile per available page. Tap one to go there. */
/* `cycle_off` is a bit per page the screensaver skips, drawn struck
   through; `held` is the tile under a finger, or PAGE_COUNT for none. */
void views_menu(canvas_t *c, const pages_t *p, unsigned cycle_off, page_t held);

/* True when a tap at display pixel (x, y) landed on a menu tile, with the
   page it stands for. */
bool views_menu_hit(canvas_t *c, const pages_t *p, int x, int y, page_t *page);

/* Today's story: the recap as wrapped prose. */
void views_story(canvas_t *c, const ud_view_t *d, float t, int64_t now_us);

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
