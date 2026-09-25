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

   A panel needs w/10+1 by h/10+1 cells, and the grid must never be smaller
   than that, because particles_init clamps rather than growing, and a clamped
   grid folds the far columns together so grains there stop separating. The
   widest panels are 320 across -- wave's, and lilly's, which the host tests
   still pour on -- at 33 columns; the tallest is watch's 240x280 portrait, at
   29 rows. The Feather's 240x135 fits inside both. It was 33x18 until watch,
   and 280 rows on that folded everything below y = 170, the lowest 110 px of
   the bottle, into a single row of cells.

   Not raised to envio's 320x480 (49 rows): she does not pour -- main.c and
   her CMakeLists both leave the sand out -- and each extra row costs 33 cells
   of 12 bytes. At 33x29 a particles_t is 38512 bytes; at 33x49 it would be
   46432. That matters on wave, which has no PSRAM, so her s_particles and
   her framebuffer share internal DRAM. A taller panel therefore still needs
   these raised, not just a bigger n. */
#define PARTICLES_CELL 10
#define PARTICLES_GRID_W 33
#define PARTICLES_GRID_H 29
#define PARTICLES_CELLS (PARTICLES_GRID_W * PARTICLES_GRID_H)

typedef struct {
    float x, y;        /* panel pixels */
    float vx, vy;      /* pixels per second */
    float ox, oy;      /* where it was when the step began */
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
   squared, in panel coordinates: +x right, +y down. Every grain ends inside
   [0,w) x [0,h), far enough from the right and bottom walls that the block
   drawn for it is whole.

   Call it once a frame with the frame's real dt; it cuts the frame into
   particles_substeps() steps itself. It has to: support climbs from the
   floor about one layer per separation pass, so a deep pile under strong
   gravity in long steps keeps compressing and never settles -- it shimmers.
   Taken as one step at 30 fps, watch at 1200 shimmered at 75 px/s with
   grains jumping 24 px a frame, and so did lilly and wave on their sides or
   tipped corner-down at their 1134 and 1147. Capping gravity would have had
   to go below about 560, since watch tipped corner-down shimmers in one
   step at 700, and the sand would pour like syrup. Cutting the step settles
   it at full strength, and costs a second solve only when the pile is
   driven hard. */
void particles_step(particles_t *s, float gx, float gy, float dt);

/* How many steps particles_step cuts a frame of `dt` into under (gx, gy):
   the fewest, up to three, that keep each step's sink |g| dt^2 under 0.4 px.
   At 30 fps that is one step lying flat, two at 1200, three at 1867 or
   below 27 fps. Each is a whole solve, so this is the sand's cost per frame;
   exposed so the tests pin it and so the caller can log it. */
int particles_substeps(float gx, float gy, float dt);

/* The most gravity the caller should drive the pile at, in px/s^2. main.c
   scales gravity per panel row, 6.67 a row, which is 1867 on watch's 280.
   The solver settles that too in three steps, but only down to 25 fps:
   tipped corner-down at 20 fps it would want four. 1200 keeps her to two
   steps at 30 fps, settles tipped any way down to 20, and still falls her
   full height in about 0.7 s against 0.55. */
#define PARTICLES_GRAVITY_MAX 1200.0f

/* Adds a tangential nudge about the centre, for the gyroscope's twist. */
void particles_swirl(particles_t *s, float rate);

/* Scatters every grain by up to `speed` pixels per second in a random
   direction. This is what a shake does: gravity says which way is down, and
   shaking is not a direction at all, it is energy. */
void particles_agitate(particles_t *s, float speed);

/* Draws each grain as a small block. Clears the canvas first. */
void particles_draw(const particles_t *s, canvas_t *c);

#endif /* PARTICLES_H */
