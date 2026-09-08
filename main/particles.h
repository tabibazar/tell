#ifndef PARTICLES_H
#define PARTICLES_H

#include "canvas.h"

#include <stdint.h>

/*
 * A bottle of particles: a few hundred grains that fall under a gravity
 * vector supplied by the caller, pile up with real depth, and keep jostling.
 * Knows nothing about accelerometers or panels, so it builds and runs on the
 * host like canvas and usagedata do.
 *
 * Grains do not collide pairwise -- that is O(n^2) and there is no budget for
 * it. Instead they are counted into a coarse grid each step and pushed down
 * the density gradient, which is enough to make the pile occupy volume rather
 * than collapsing onto the boundary line. Without it every grain ends up at
 * the same wall and the animation is over the moment they arrive.
 */

#define PARTICLES_MAX 1024

/* The density grid. One cell per CELL pixels square, sized for the largest
   panel this runs on. */
#define PARTICLES_CELL 10
#define PARTICLES_GRID_W 40
#define PARTICLES_GRID_H 24

typedef struct {
    float x, y;        /* panel pixels */
    float vx, vy;      /* pixels per second */
    uint16_t colour;
} particle_t;

typedef struct {
    particle_t p[PARTICLES_MAX];
    int n;
    int w, h;
    uint32_t rng;
    int gw, gh;                                    /* grid cells in use */
    uint8_t grid[PARTICLES_GRID_W * PARTICLES_GRID_H];
} particles_t;

/* Scatters `n` particles over a w x h panel. `n` is clamped to PARTICLES_MAX. */
void particles_init(particles_t *s, int n, int w, int h, uint32_t seed);

/* Advances by `dt` seconds under gravity (gx, gy) in pixels per second
   squared, in panel coordinates: +x right, +y down. */
void particles_step(particles_t *s, float gx, float gy, float dt);

/* Adds a tangential nudge about the centre, for the gyroscope's twist. */
void particles_swirl(particles_t *s, float rate);

/* Draws each particle as a small block. Clears the canvas first. */
void particles_draw(const particles_t *s, canvas_t *c);

#endif /* PARTICLES_H */
