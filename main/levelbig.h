#ifndef LEVELBIG_H
#define LEVELBIG_H

#include "canvas.h"

#include <stdbool.h>

/*
 * envio's own rendering of the Feather's spirit-level game: same inputs as
 * level.c's level_draw (the game and its state live there, in main.c's
 * level_step), drawn big and centred for her tall 320x480 panel instead of
 * the Feather's small one. Hardware-free, like level.c, so it is tested on
 * the host.
 */
void levelbig_draw(canvas_t *c, float tilt_x_deg, float tilt_y_deg,
                   float hold_s, float last_s, float prev_s, float best_s,
                   bool armed);

#endif /* LEVELBIG_H */
