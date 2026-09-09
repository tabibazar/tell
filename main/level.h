#ifndef LEVEL_H
#define LEVEL_H

#include "canvas.h"

#include <stdbool.h>

/*
 * A bullseye spirit level for a board lying flat: how far the panel's own two
 * axes are off horizontal. Both read zero on a true surface, which is what
 * makes it a bullseye -- an earlier version measured roll and pitch about an
 * upright board, and lying flat that reads ninety degrees of pitch and a roll
 * made of nothing but noise.
 *
 * Knows nothing about accelerometers -- it is handed two angles in degrees --
 * so it draws on the host like every other view here.
 */

/* Full scale: the outermost ring. Beyond this the bubble stops at the rim. */
#define LEVEL_FULL_SCALE_DEG 15.0f

/* Within this of true, on both axes, it calls itself level. */
#define LEVEL_TOLERANCE_DEG 0.7f

bool level_is_true(float tilt_x_deg, float tilt_y_deg);

/* Positive means that end of the axis is the low one: +x is the right edge
   down, +y is the far edge down. */
/* `note` is printed small at the foot of the readout, or NULL. */
void level_draw(canvas_t *c, float tilt_x_deg, float tilt_y_deg, const char *note);

#endif /* LEVEL_H */
