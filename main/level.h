#ifndef LEVEL_H
#define LEVEL_H

#include "canvas.h"

#include <stdbool.h>

/*
 * A bullseye spirit level. Two angles, because a board standing on a desk can
 * be out of true in two ways at once: rolled, so its bottom edge is not
 * horizontal, and pitched, so its face is not vertical.
 *
 * Knows nothing about accelerometers -- it is handed two angles in degrees --
 * so it draws on the host like every other view here.
 */

/* Full scale: the outermost ring. Beyond this the bubble stops at the rim. */
#define LEVEL_FULL_SCALE_DEG 15.0f

/* Within this of true, on both axes, it calls itself level. */
#define LEVEL_TOLERANCE_DEG 0.7f

bool level_is_true(float roll_deg, float pitch_deg);

/* Rolled right is positive, pitched away from you is positive. */
void level_draw(canvas_t *c, float roll_deg, float pitch_deg);

#endif /* LEVEL_H */
