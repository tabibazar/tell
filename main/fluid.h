#ifndef FLUID_H
#define FLUID_H

/*
 * Water in a bottle, for watch: a few hundred drops that hold together, find
 * a level surface, and slosh and splash when the bottle moves.
 *
 * Clavet, Beaudoin and Poulin's double-density relaxation ("Particle-based
 * Viscoelastic Fluid Simulation", 2005): each drop is pushed by the pressure
 * of its neighbours' density against a rest density, and a second, "near"
 * density keeps drops from clumping onto each other and gives the surface its
 * tension. Velocities come back from how far relaxation moved each drop, so
 * the liquid keeps its momentum -- it overshoots, rolls up the walls and
 * throws drops, where the sand (particles.c) only ever settles.
 *
 * Drawn as the drops themselves, beads like the sand's (Reza, 2026-10-03:
 * "individual water droplets like sand but they behave like a liquid"):
 * each one of the sand's six colours with a highlight, paler the faster it
 * moves, so a splash shows.
 *
 * Gravity is whatever the accelerometer says, unfiltered: a shake is the
 * bottle's own acceleration, which is exactly what makes real water slosh.
 * Pure C over a canvas, so it runs and is rendered on the host.
 */
#include <stdint.h>
#include "canvas.h"

#define FLUID_MAX     420
#ifndef FLUID_RADIUS_PX
#define FLUID_RADIUS_PX 18      /* interaction radius, px */
#endif
#define FLUID_RADIUS  ((float)FLUID_RADIUS_PX)
#define FLUID_GRID_W  (240 / FLUID_RADIUS_PX + 2)   /* cells of FLUID_RADIUS */
#define FLUID_GRID_H  (280 / FLUID_RADIUS_PX + 2)
#define FLUID_NEIGH   32        /* neighbours kept per drop per step */

typedef struct {
    float x[FLUID_MAX], y[FLUID_MAX];
    float vx[FLUID_MAX], vy[FLUID_MAX];
    float px[FLUID_MAX], py[FLUID_MAX];       /* positions at the step's start */
    uint8_t colour[FLUID_MAX];                /* one of the sand's six */
    int n, w, h;
    int16_t head[FLUID_GRID_W * FLUID_GRID_H];
    int16_t next[FLUID_MAX];
    uint8_t nn[FLUID_MAX];
    int16_t nb[FLUID_MAX][FLUID_NEIGH];
} fluid_t;

/* How many drops for a w x h bottle: 300 on watch, about a quarter full --
   as many as she can move at full speed. */
int fluid_for(int w, int h);

/* Drops in a block at the bottom, at rest. w and h at most 240 x 280. */
void fluid_init(fluid_t *f, int n, int w, int h, uint32_t seed);

/* Advances `dt` seconds under (gx, gy) px/s^2, +x right, +y down, in fixed
   substeps of 1/60 s (at most 3 a call). */
void fluid_step(fluid_t *f, float gx, float gy, float dt);

/* What drawing needs, copied out after a step: so one core can draw the
   last step while the other computes the next (main.c on watch). */
typedef struct {
    int n;
    float x[FLUID_MAX], y[FLUID_MAX];
    uint8_t colour[FLUID_MAX];
    uint8_t speed[FLUID_MAX];         /* 0 still .. 3 spray */
} fluid_view_t;

void fluid_snapshot(const fluid_t *f, fluid_view_t *v);
void fluid_draw(const fluid_view_t *v, canvas_t *c);

#endif /* FLUID_H */
