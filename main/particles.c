#include "particles.h"

#include "palette.h"

#include <stdbool.h>

/* Bounce loses this much speed, so grains do not ring off the walls. */
#define RESTITUTION 0.35f
/* Drag per second. High enough to keep the bottle from becoming a blender,
   low enough that a tilt still sloshes for a second or two afterwards. */
#define DRAG 0.55f
/* How hard a crowded cell pushes its neighbours away, in pixels per second
   squared per surplus grain. This is what gives the pile depth: without it
   every grain settles onto the same boundary line and nothing moves again.
   Against GRAVITY_PX in main.c it sets how deep the heap stands: higher fills
   more of the bottle, but past about 500 the pile stops settling and starts
   boiling, because the grid is coarse enough that the term fights itself. */
#define PRESSURE 350.0f
/* A per-frame nudge, in pixels per second. Small: the drag above turns it
   into a shimmer of about ten pixels a second, so a settled heap keeps
   shifting instead of freezing, which is both nicer to watch and better for
   a panel that shows this for hours. */
#define JITTER 3.0f

#define PARTICLE_SIZE 3

static uint32_t next_rand(particles_t *s)
{
    s->rng = s->rng * 1103515245u + 12345u;
    return s->rng >> 16;
}

static float frand(particles_t *s, float lo, float hi)
{
    return lo + (float)(next_rand(s) % 10000u) / 10000.0f * (hi - lo);
}

void particles_init(particles_t *s, int n, int w, int h, uint32_t seed)
{
    if (n > PARTICLES_MAX) n = PARTICLES_MAX;
    if (n < 0) n = 0;
    s->n = n;
    s->w = w;
    s->h = h;
    s->rng = seed ? seed : 1u;

    /* Six colours from the project's Okabe-Ito set, so the animation belongs
       to the same palette as the charts. */
    static const uint16_t colours[6] = {
        PAL_A0, PAL_A1, PAL_A2, PAL_A3, PAL_A4, PAL_A5
    };

    for (int i = 0; i < n; i++) {
        s->p[i].x = frand(s, 1.0f, (float)w - 1.0f);
        s->p[i].y = frand(s, 1.0f, (float)h - 1.0f);
        s->p[i].vx = frand(s, -20.0f, 20.0f);
        s->p[i].vy = frand(s, -20.0f, 20.0f);
        s->p[i].colour = colours[next_rand(s) % 6u];
    }
}

static void bounce(float *pos, float *vel, float limit)
{
    if (*pos < 0.0f) {
        *pos = 0.0f;
        *vel = -*vel * RESTITUTION;
    } else if (*pos > limit) {
        *pos = limit;
        *vel = -*vel * RESTITUTION;
    } else {
        return;
    }
}

/* Counts the grains into the coarse grid, so the step below can read a
   density from it. Saturating, because a cell that holds more than 255 grains
   is already as crowded as the gradient can express. */
static void bin(particles_t *s)
{
    s->gw = s->w / PARTICLES_CELL + 1;
    s->gh = s->h / PARTICLES_CELL + 1;
    if (s->gw > PARTICLES_GRID_W) s->gw = PARTICLES_GRID_W;
    if (s->gh > PARTICLES_GRID_H) s->gh = PARTICLES_GRID_H;

    for (int i = 0, n = s->gw * s->gh; i < n; i++) s->grid[i] = 0;

    for (int i = 0; i < s->n; i++) {
        int cx = (int)s->p[i].x / PARTICLES_CELL;
        int cy = (int)s->p[i].y / PARTICLES_CELL;
        if (cx < 0) cx = 0; else if (cx >= s->gw) cx = s->gw - 1;
        if (cy < 0) cy = 0; else if (cy >= s->gh) cy = s->gh - 1;
        uint8_t *cell = &s->grid[cy * s->gw + cx];
        if (*cell < 255) (*cell)++;
    }
}

/* The count in a cell, false when there is no such cell. Off the grid is not
   "empty": treating it as empty would let a wall pull grains through it, and
   treating it as full would fire them back across the panel. The walls are
   the business of bounce(); here they simply do not vote. */
static bool cell(const particles_t *s, int cx, int cy, int *out)
{
    if (cx < 0 || cy < 0 || cx >= s->gw || cy >= s->gh) return false;
    *out = s->grid[cy * s->gw + cx];
    return true;
}

/* Where a crowded cell wants to send this grain: toward each neighbour that
   holds fewer, in proportion to how many fewer. Comparing a cell against its
   own contents is the point -- an earlier version compared only the two
   neighbours, so a cell packed with sixteen grains and empty ones either
   side felt nothing at all, and the whole pile sat one cell deep. */
static void pressure(const particles_t *s, int cx, int cy, float *fx, float *fy)
{
    int self;
    if (!cell(s, cx, cy, &self)) { *fx = *fy = 0.0f; return; }

    float ax = 0.0f, ay = 0.0f;
    int d;
    if (cell(s, cx - 1, cy, &d) && self > d) ax -= (float)(self - d);
    if (cell(s, cx + 1, cy, &d) && self > d) ax += (float)(self - d);
    if (cell(s, cx, cy - 1, &d) && self > d) ay -= (float)(self - d);
    if (cell(s, cx, cy + 1, &d) && self > d) ay += (float)(self - d);
    *fx = ax;
    *fy = ay;
}

void particles_step(particles_t *s, float gx, float gy, float dt)
{
    /* Drag as a per-second multiplier applied linearly over dt. At the frame
       rates this runs at, dt is small enough that the approximation is
       indistinguishable from the exponential and much cheaper. */
    float damp = 1.0f - (1.0f - DRAG) * dt;
    if (damp < 0.0f) damp = 0.0f;

    float maxx = (float)s->w;
    float maxy = (float)s->h;

    bin(s);

    for (int i = 0; i < s->n; i++) {
        particle_t *p = &s->p[i];

        float px, py;
        pressure(s, (int)p->x / PARTICLES_CELL, (int)p->y / PARTICLES_CELL,
                 &px, &py);

        p->vx = (p->vx + (gx + px * PRESSURE) * dt) * damp;
        p->vy = (p->vy + (gy + py * PRESSURE) * dt) * damp;

        /* Never quite still. */
        p->vx += (frand(s, -1.0f, 1.0f)) * JITTER;
        p->vy += (frand(s, -1.0f, 1.0f)) * JITTER;

        p->x += p->vx * dt;
        p->y += p->vy * dt;
        bounce(&p->x, &p->vx, maxx);
        bounce(&p->y, &p->vy, maxy);
    }
}

void particles_swirl(particles_t *s, float rate)
{
    float cx = (float)s->w / 2.0f;
    float cy = (float)s->h / 2.0f;
    for (int i = 0; i < s->n; i++) {
        float rx = s->p[i].x - cx;
        float ry = s->p[i].y - cy;
        s->p[i].vx += -ry * rate;
        s->p[i].vy +=  rx * rate;
    }
}

void particles_agitate(particles_t *s, float speed)
{
    if (speed <= 0.0f) return;
    for (int i = 0; i < s->n; i++) {
        s->p[i].vx += frand(s, -speed, speed);
        s->p[i].vy += frand(s, -speed, speed);
    }
}

void particles_draw(const particles_t *s, canvas_t *c)
{
    canvas_clear(c);
    for (int i = 0; i < s->n; i++)
        canvas_fill_rect(c, (int)s->p[i].x, (int)s->p[i].y,
                         PARTICLE_SIZE, PARTICLE_SIZE, s->p[i].colour);
}
