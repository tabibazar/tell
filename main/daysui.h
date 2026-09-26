#ifndef DAYSUI_H
#define DAYSUI_H

/*
 * watch's Days page: how loud speaker's room has been, day by day, and how
 * today is going against a usual day. speaker prints a "days:" line once a
 * minute -- up to 35 days ending today, each its LAeq and L90 -- and the Mac
 * relays it to watch's USB serial as "!noisedays" lines, which main.c merges
 * by date into the struct below. One 240x280 portrait page, top to bottom:
 *
 *   - TODAY SO FAR, and today's running LAeq large, in the ring's colour for
 *     it (noiseui_colour, the Sound page's own), dBA beside it and EST over
 *     that while speaker's microphone is uncalibrated. The caption is there
 *     because today is not over: the figure is the day so far, not the day;
 *   - how that compares with a usual day: "+3.2 dB louder" / "-1.5 dB
 *     quieter" / "about the same" (under 1 dB either way), and against what:
 *     "than a usual Friday (4)" -- the energy mean of up to the four latest
 *     earlier Fridays that have a level -- or, with none of those yet, "than
 *     recent days (n)", the energy mean of up to the seven latest earlier
 *     days that have one; with no earlier day at all, "baseline starts
 *     tomorrow";
 *   - BACKGROUND and a level: the median L90 of up to the seven latest
 *     earlier days that have one (today's L90 when no earlier day has), the
 *     room with nothing happening in it;
 *   - the last seven days by the calendar as bars on a fixed 30..80 dBA
 *     scale, each in the ring's colour for its LAeq with its figure over it,
 *     the ring's four stops dotted faint between them as on the Sound page,
 *     Mo..Su under them, today's in gold over a small lozenge. A day with no
 *     level after the first that has one is hatched, as the Sound page
 *     hatches its gaps: no figure from speaker is not a quiet day.
 *
 * Nothing received yet and the page says NO DATA, waiting for speaker, with
 * no figure anywhere. Stale -- main.c's call: no line for a while, or the
 * line's today is not the watch's -- and the page is drawn as it was but
 * dimmed, the caption OUT OF DATE in place of TODAY SO FAR: the past days are
 * still true, today's figure has stopped being.
 *
 * Energy means because decibels do not average: a 70 dBA day and a 40 dBA
 * day are a 67 dBA pair, not a 55 one -- what the ear and speaker's LAeq both
 * add up. The median for the background because one night the fridge was
 * off should not move it.
 *
 * Pure: no clock, no hardware, no globals from main.c; this only draws the
 * struct, so the page can be rendered and checked on the host. The palette,
 * ground and type are the Sound page's (noiseui.c), and the level's colour is
 * the only colour on it.
 */
#include "canvas.h"

#include <stdbool.h>
#include <stdint.h>

#define DAYSUI_WIDTH   240
#define DAYSUI_HEIGHT  280

/* speaker's days line carries up to 35 days ending today: five weeks, so
   four earlier same weekdays and today. */
#define DAYSUI_DAYS    35

/* The last seven days, as bars. */
#define DAYSUI_BARS    7

/*
 * One day. The date is the only thing that says which day it is: its weekday
 * is worked out from it (daysui_weekday), never carried beside it, so the two
 * cannot disagree. A date that is not a date -- month 0 or 13, day 0 or 31
 * September, a year outside 2000..2199 -- is a day that is nowhere: no bar,
 * no part of any baseline.
 */
typedef struct {
    uint16_t year;      /* 2026 */
    uint8_t  month;     /* 1..12 */
    uint8_t  day;       /* 1..31 */
    float    laeq;      /* dBA, the day's LAeq; NaN for "--" */
    float    l90;       /* dBA, the level exceeded 90 % of the day; NaN for "--" */
    bool     today;     /* the running day, speaker's "*": partial */
} daysui_day_t;

typedef struct {
    bool have_data;     /* a days line has arrived since boot */
    bool stale;         /* ...but not lately, or not for today: drawn dimmed */
    bool calibrated;    /* false: speaker's levels are estimates, "est" */
    int  n;             /* days in day[], oldest first; clamped to 0..DAYSUI_DAYS */
    daysui_day_t day[DAYSUI_DAYS];
} daysui_t;

/* What today is measured against. */
typedef enum {
    DAYSUI_BASE_NONE = 0,   /* no earlier day has a level: "baseline starts tomorrow" */
    DAYSUI_BASE_WEEKDAY,    /* earlier same weekdays: "than a usual Friday (n)" */
    DAYSUI_BASE_RECENT,     /* earlier days, any weekday: "than recent days (n)" */
} daysui_base_kind_t;

typedef struct {
    daysui_base_kind_t kind;
    float level;        /* dBA, the energy mean; NaN for NONE */
    int   n;            /* days in it: 1..4 for WEEKDAY, 1..7 for RECENT, 0 for NONE */
    int   weekday;      /* today's, 0 Monday .. 6 Sunday; -1 when there is no today */
} daysui_base_t;

/*
 * The whole page, onto a canvas of DAYSUI_WIDTH x DAYSUI_HEIGHT (any other
 * size is drawn at the same place, clipped). A NULL struct draws the ground
 * alone. Nothing lands outside the framebuffer whatever the struct holds:
 * NaN, infinities, nonsense dates and a count out of range included.
 */
void daysui_draw(canvas_t *c, const daysui_t *s);

/*
 * The baseline today is compared with. Today is the last day flagged today;
 * the days it may draw on are those dated strictly before it (with no today,
 * every day), walked back from the latest: the first up to four on today's
 * weekday that have a level, and if there are none, the first up to seven of
 * any weekday that have one. Today's own level is never part of it.
 */
daysui_base_t daysui_baseline(const daysui_t *s);

/* The background: the median L90 of up to the seven latest days before today
   that have one; today's L90 if none has; NaN if today has none either.
   `n` (may be NULL) gets how many days it is the median of, 0 for today's. */
float daysui_background(const daysui_t *s, int *n);

/* The energy mean of `n` levels in dBA: 10 log10 of the mean of 10^(L/10).
   NaN for n <= 0. Public so the test can hold the baselines to it. */
float daysui_energy_mean(const float *dba, int n);

/* The weekday of a date, 0 Monday .. 6 Sunday -- NOT C's tm_wday, which
   starts on Sunday -- or -1 when it is not a date as daysui_day_t says. */
int daysui_weekday(int year, int month, int day);

#endif /* DAYSUI_H */
