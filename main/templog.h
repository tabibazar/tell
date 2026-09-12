#ifndef TEMPLOG_H
#define TEMPLOG_H

#include "canvas.h"

#include <stdbool.h>

/*
 * A rolling record of two temperatures, and a line chart of it.
 *
 * The two are the ESP32's own die and the DS3231's crystal. They are worth
 * seeing together: the die is the board working, the crystal is very nearly
 * the room, and the gap between them is how much of the board's own heat is
 * reaching the chip that is supposed to be compensating for the room's.
 *
 * Both the record and the drawing are here, free of sensors and panels, so
 * the ring arithmetic and -- more to the point -- the bounds of every pixel
 * the chart paints can be checked on the host.
 */

/* One sample per two pixels across a 320-wide panel. */
#define TEMPLOG_MAX 160

typedef struct {
    float die[TEMPLOG_MAX];
    float xtal[TEMPLOG_MAX];
    bool  has_xtal[TEMPLOG_MAX];
    int   n;          /* how many samples are held, up to TEMPLOG_MAX */
    int   head;       /* where the next one goes */
} templog_t;

void templog_init(templog_t *t);

/* Adds a sample, dropping the oldest once full. `xtal` is ignored unless
   `have_xtal`, so a board with no clock chip still charts its own die. */
void templog_add(templog_t *t, float die, bool have_xtal, float xtal);

int templog_count(const templog_t *t);

/* Oldest first, so index 0 is the left of the chart. */
float templog_die(const templog_t *t, int i);
bool  templog_xtal(const templog_t *t, int i, float *out);

/*
 * The lowest and highest of everything held, which is what the chart is
 * scaled to. False when there is nothing to scale yet.
 */
bool templog_range(const templog_t *t, float *lo, float *hi);

/* Draws the chart, the two series, and marks the extremes. Clears first. */
void templog_draw(const templog_t *t, canvas_t *c);

#endif /* TEMPLOG_H */
