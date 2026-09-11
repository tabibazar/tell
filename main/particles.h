#ifndef PARTICLES_H
#define PARTICLES_H

#include "canvas.h"

#include <stdint.h>

/*
 * A bottle of liquid: a few hundred grains that pour the way the caller says
 * gravity points, settle to a level surface, and slosh when the bottle moves.
 * Knows nothing about accelerometers or panels, so it builds and runs on the
 * host like canvas and usagedata do.
 *
 * Grains genuinely push each other apart. An earlier version approximated
 * that with a density grid, which is cheaper and works for a heap of sand,
 * but could not hold a column of liquid up: a grain resting on the body sat
 * in a cell no fuller than any other, felt nothing, and fell through. Five
 * grains counted on an eight-pixel cell is too coarse a number to carry a
 * hydrostatic gradient. So they are separated pairwise instead, against a
 * bucket grid so each grain only ever looks at its immediate neighbours.
 */

#define PARTICLES_MAX 900

/* How far apart grains hold each other. Sets how much of the bottle a given
   number of them fills. */
#define PARTICLES_RADIUS 5.0f

/* The bucket grid, one cell per CELL pixels. CELL must be at least twice the
   radius, so every grain close enough to matter is in one of the nine cells
   around this one.

   The grid is sized for the largest panel that runs this, lilly's 320x170:
   320/10+1 by 170/10+1, which also covers the Feather's 240x135. It must
   never be smaller than the panel it is given,
   because particles_init clamps rather than growing, and a clamped grid folds
   the far columns together so grains there stop separating. A panel wider
   than 320 therefore needs these raised, not just a bigger n. */
#define PARTICLES_CELL 10
#define PARTICLES_GRID_W 33
#define PARTICLES_GRID_H 18
#define PARTICLES_CELLS (PARTICLES_GRID_W * PARTICLES_GRID_H)

typedef struct {
    float x, y;        /* panel pixels */
    float vx, vy;      /* pixels per second */
    float ox, oy;      /* where it was when the frame began */
    uint16_t colour;
} particle_t;

typedef struct {
    particle_t p[PARTICLES_MAX];
    int n;
    int w, h;
    uint32_t rng;

    int gw, gh;                         /* bucket cells in use */
    uint16_t head[PARTICLES_CELLS + 1]; /* where each cell's grains start */
    uint16_t fill[PARTICLES_CELLS];     /* scratch while bucketing */
    uint16_t order[PARTICLES_MAX];      /* grain indices, grouped by cell */
    float vgx[PARTICLES_CELLS];         /* mean velocity, for viscosity */
    float vgy[PARTICLES_CELLS];
} particles_t;

/* How many grains a w x h panel wants, so the bottle looks equally full on
   any of them. The density is the one the Feather's 240x135 was tuned to by
   eye at 190 grains; lilly's 320x170 comes out at 320. Clamped to
   PARTICLES_MAX. */
#define PARTICLES_PIXELS_EACH 170
int particles_for(int w, int h);

/* Scatters `n` grains over a w x h panel. `n` is clamped to PARTICLES_MAX. */
void particles_init(particles_t *s, int n, int w, int h, uint32_t seed);

/* Advances by `dt` seconds under gravity (gx, gy) in pixels per second
   squared, in panel coordinates: +x right, +y down. */
void particles_step(particles_t *s, float gx, float gy, float dt);

/* Adds a tangential nudge about the centre, for the gyroscope's twist. */
void particles_swirl(particles_t *s, float rate);

/* Scatters every grain by up to `speed` pixels per second in a random
   direction. This is what a shake does: gravity says which way is down, and
   shaking is not a direction at all, it is energy. */
void particles_agitate(particles_t *s, float speed);

/* Draws each grain as a small block. Clears the canvas first. */
void particles_draw(const particles_t *s, canvas_t *c);

#endif /* PARTICLES_H */
