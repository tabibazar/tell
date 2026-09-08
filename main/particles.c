#include "particles.h"

#include "palette.h"

/* Bounce loses this much speed, so the pile settles instead of ringing. */
#define RESTITUTION 0.45f
/* Drag per second, so motion dies away when gravity is removed. */
#define DRAG 0.90f
/* Below this speed a particle against a wall is simply stopped, which is what
   keeps a settled heap from shivering on a noisy accelerometer. */
#define SLEEP_SPEED 6.0f

#define PARTICLE_SIZE 2

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
    if (*vel < SLEEP_SPEED && *vel > -SLEEP_SPEED) *vel = 0.0f;
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

    for (int i = 0; i < s->n; i++) {
        particle_t *p = &s->p[i];
        p->vx = (p->vx + gx * dt) * damp;
        p->vy = (p->vy + gy * dt) * damp;
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

void particles_draw(const particles_t *s, canvas_t *c)
{
    canvas_clear(c);
    for (int i = 0; i < s->n; i++)
        canvas_fill_rect(c, (int)s->p[i].x, (int)s->p[i].y,
                         PARTICLE_SIZE, PARTICLE_SIZE, s->p[i].colour);
}
