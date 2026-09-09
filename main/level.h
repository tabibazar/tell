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

/* Full scale: the outermost ring. Beyond this the bubble stops at the rim.
   Five degrees, not fifteen: this levels a desk, not a driveway, and at
   fifteen a whole degree moved the bubble less than four pixels -- under the
   dot itself, so the dial looked centred while the number honestly read 1.0.
   The number was right and the picture could not show it. */
#define LEVEL_FULL_SCALE_DEG 5.0f

/* Within this of true, on both axes, it calls itself level.

   Found by playing rather than reasoned about, which is the only way to set a
   difficulty: three quarters of a degree was a real level's tolerance and
   unpleasantly hard, two degrees was too easy, so it sits between them. Still
   flat enough to trust a desk to. */
#define LEVEL_TOLERANCE_DEG 1.2f

bool level_is_true(float tilt_x_deg, float tilt_y_deg);

/* Positive means that end of the axis is the low one: +x is the right edge
   down, +y is the far edge down. */
/*
 * `hold_s` is how long it has been held true for, `last_s` the run that just
 * ended and `best_s` the longest there has ever been -- holding a board flat
 * by hand is harder than it sounds, so the level doubles as a game, and the
 * run you just missed by is the one you want to see, and `prev_s` the one
 * before that, so a second player can see what the player before them
 * managed. Both outlive the power.
 *
 * `armed` is whether the board has been off level since the page opened. The
 * clock does not run until it has: a board left on a level table holds for
 * ever and would take the record by being ignored.
 */
void level_draw(canvas_t *c, float tilt_x_deg, float tilt_y_deg,
                float hold_s, float last_s, float prev_s, float best_s,
                bool armed);

#endif /* LEVEL_H */
