#ifndef PIP_H
#define PIP_H

#include "canvas.h"

#include <stdbool.h>

/*
 * Pip, visio's desk familiar: a face that watches which way is down and
 * startles when the board is shaken.
 *
 * Free of hardware -- it is fed gravity and a shake flag and decides where the
 * eyes look and how wide they are, then draws itself to a canvas -- so it runs
 * and is tested on the host, the way the level and the sand are. The board
 * reads the QMI8658 and turns it into gravity + shake; Pip never sees a sensor.
 */

typedef struct {
    float look_x, look_y;   /* smoothed pupil offset, each -1..1 */
    float startle;          /* 0 calm .. 1 just startled; decays on its own */
    float blink;            /* 0 eyes open .. 1 shut */
    float since_blink_s;    /* seconds since the last blink began */
    float blink_left_s;     /* seconds left in the current blink, 0 when open */
} pip_t;

void pip_init(pip_t *p);

/*
 * One step. gx, gy are gravity in panel coordinates -- the direction "down" --
 * each about -1..1 at full tilt, as the board's gravity_from gives them.
 * `shaken` is true on the frame a shake is recognised. dt is seconds since the
 * last call.
 */
void pip_update(pip_t *p, float gx, float gy, bool shaken, float dt);

/* Draws Pip filling the canvas. */
void pip_draw(canvas_t *c, const pip_t *p);

#endif /* PIP_H */
