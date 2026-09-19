#ifndef BUBBLELEVEL_H
#define BUBBLELEVEL_H
#include "canvas.h"
#include <stdbool.h>

/* A spirit-level game, free of hardware like pip/level: fed gravity in canvas
   coords (gx,gy ~ -1..1, +y is down, as gravity_from gives) and a timestep, it
   floats a bubble and runs the "hold it level" game. The board reads the
   QMI8658; this never sees a sensor. */
#define BUBBLE_LEVELS 3          /* tolerance tightens each level */

typedef struct {
    float bx, by;                /* bubble offset from centre, canvas px */
    float held_s;                /* seconds held within tolerance this attempt */
    int   level;                 /* 0..BUBBLE_LEVELS-1 */
    bool  won;
    float roll_deg, pitch_deg;   /* current tilt, for the readout */
} bubble_t;

void bubble_init(bubble_t *b);
void bubble_reset(bubble_t *b);  /* restart the current level (tap) */
void bubble_update(bubble_t *b, float gx, float gy, float dt);
void bubble_draw(canvas_t *c, const bubble_t *b);
#endif
