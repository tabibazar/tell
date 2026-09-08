#ifndef PARTICLES_H
#define PARTICLES_H

#include "canvas.h"

#include <stdint.h>

/*
 * A few hundred particles falling under a gravity vector supplied by the
 * caller. Knows nothing about accelerometers or panels, so it builds and runs
 * on the host like canvas and usagedata do.
 *
 * Particles do not collide with each other. A few hundred of them at twenty
 * frames a second has no budget for it, and a heap that slumps under gravity
 * reads correctly without it.
 */

#define PARTICLES_MAX 240

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
