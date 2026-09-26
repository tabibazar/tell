#ifndef NOISEUI_H
#define NOISEUI_H

/*
 * watch's Sound page: how loud speaker's room is, read off speaker's 1 Hz
 * serial line, which the Mac relays to watch's USB serial as "!noise ...".
 * One 240x280 portrait page, top to bottom:
 *
 *   - the level now, in dBA, large: LAF, the fast level -- what speaker's
 *     ring fills to -- rounded to a whole decibel;
 *   - coloured as speaker's ring is coloured, from LAeq,3s through green at
 *     45 dBA and below, yellow at 55, orange at 65 and red at 75 and above,
 *     ring.c's own gradient (ring_colour_for), so the watch on a wrist and
 *     the ring on the shelf are never two colours for one room;
 *   - the word for the same LAeq,3s, in that colour: QUIET under 45,
 *     MODERATE to 60, LOUD to 75, VERY LOUD from 75. The word and the colour
 *     come from one number and cannot disagree; the big figure is the
 *     faster one, as the ring's fill is, and may run ahead of them for a
 *     second or two when a door slams;
 *   - EST beside the unit while speaker's microphone is uncalibrated;
 *   - today's running average (LAeq since midnight, speaker's figure);
 *   - the last hour as a strip on a fixed 30..90 dBA scale, one column per
 *     two of the history's 10 s points, with the ring's four stops drawn
 *     across it as faint lines.
 *
 * No line for 10 s and the page says NO SIGNAL where the number was, and
 * shows no number anywhere -- not the level, not today's -- rather than one
 * that has stopped being true. The strip stays: it is history, and says so.
 *
 * Every word and figure is Inter, anti-aliased (aafont.h); the marks are
 * vector.c's, blended in linear light as the type is. The palette is the
 * watch face's: a near-black navy ground, cream for what is printed, gold
 * for labels, and colour only for the level.
 *
 * Pure: no clock, no hardware, no globals from main.c. main.c keeps the
 * history and the time of the last line and fills the struct below; this
 * only draws it, so the page can be rendered and checked on the host.
 */
#include "canvas.h"

#include <stdbool.h>

#define NOISEUI_WIDTH   240
#define NOISEUI_HEIGHT  280

/* One hour at one point per 10 s. [NOISEUI_POINTS - 1] is the latest; each
   point is the level over its 10 s in dBA -- the energy mean of the laeq3
   values that arrived in it is right, the last of them near enough -- and a
   10 s with no line in it is a point with valid false, drawn as a gap. */
#define NOISEUI_POINTS  360

typedef enum {
    NOISEUI_QUIET = 0,   /* under 45 dBA */
    NOISEUI_MODERATE,    /* 45 to under 60 */
    NOISEUI_LOUD,        /* 60 to under 75 */
    NOISEUI_VERY_LOUD,   /* 75 and up */
} noiseui_state_t;

typedef struct {
    bool  have_signal;   /* a "!noise" line within the last 10 s */
    float laf;           /* dBA, the fast level: the big number */
    float laeq3;         /* dBA, LAeq over 3 s: the colour and the word */
    float today;         /* dBA, LAeq since midnight; NAN when speaker says "--" */
    bool  calibrated;    /* false: speaker's level is an estimate, "est" */
    float hist[NOISEUI_POINTS];        /* dBA; NaN counts as a gap */
    bool  hist_valid[NOISEUI_POINTS];
} noiseui_t;

/*
 * The whole page, onto a canvas of NOISEUI_WIDTH x NOISEUI_HEIGHT (any other size is
 * drawn at the same place, clipped). The state -- the word and the colour --
 * is worked out here from laeq3 with noiseui_state_for, never taken from the
 * caller, so it cannot say one thing while the number says another. A NULL
 * struct draws the ground alone. Nothing lands outside the framebuffer,
 * whatever the numbers are: NaN, infinities and nonsense included.
 */
void noiseui_draw(canvas_t *c, const noiseui_t *s);

/* The word's band for a level in dBA, by the thresholds above. Anything not
   a number (NaN) is QUIET; the page shows no word for it. */
noiseui_state_t noiseui_state_for(float dba);

/* The word itself: "QUIET", "MODERATE", "LOUD" or "VERY LOUD". */
const char *noiseui_state_name(noiseui_state_t st);

/* The level's colour as the page draws it: ring_colour_for, taken from
   linear light to RGB565. Public so the test can hold it to the ring. */
uint16_t noiseui_colour(float dba);

#endif /* NOISEUI_H */
