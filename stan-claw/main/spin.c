#include "spin.h"

#include <math.h>

#define FRICTION   2.6f       /* per second: how fast a flick dies away */
#define SNAP_BELOW 1.2f       /* cards/s: slower than this, it settles on a card */
#define SNAP_RATE  14.0f      /* how briskly it settles */
#define MAX_VEL    18.0f

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void spin_init(spin_t *s, int n, int index)
{
    s->n = n > 0 ? n : 1;
    s->pos = s->grab_pos = (float)(index < 0 ? 0 : index >= s->n ? s->n - 1 : index);
    s->vel = 0;
    s->held = s->moving = false;
}

void spin_grab(spin_t *s)
{
    s->held = true;
    s->moving = true;
    s->vel = 0;
    s->grab_pos = s->pos;
}

void spin_drag(spin_t *s, float dx_px)
{
    float p = s->grab_pos - dx_px / SPIN_CARD_PX;
    float hi = (float)(s->n - 1);
    /* Past either end it gives, but only a third as much: a rubber band. */
    if (p < 0) p /= 3.0f;
    else if (p > hi) p = hi + (p - hi) / 3.0f;
    s->pos = p;
}

void spin_release(spin_t *s, float vx_px_s)
{
    s->held = false;
    s->vel = clampf(-vx_px_s / SPIN_CARD_PX, -MAX_VEL, MAX_VEL);
    s->moving = true;
}

void spin_go(spin_t *s, int index)
{
    s->grab_pos = s->pos;
    s->pos = s->pos;          /* the step glides it */
    s->vel = 0;
    s->held = false;
    s->moving = true;
    /* Aim: one card's worth of velocity per card of distance, then snapping
       finishes it; for a neighbour that is a short glide. */
    float d = (float)index - s->pos;
    s->vel = d * 3.2f;
}

int spin_index(const spin_t *s)
{
    int i = (int)lroundf(s->pos);
    return i < 0 ? 0 : i >= s->n ? s->n - 1 : i;
}

bool spin_step(spin_t *s, float dt)
{
    if (s->held || !s->moving) return s->moving;
    float hi = (float)(s->n - 1);
    if (fabsf(s->vel) >= SNAP_BELOW) {
        s->pos += s->vel * dt;
        s->vel *= expf(-FRICTION * dt);
        if (s->pos < 0 || s->pos > hi) { s->vel = 0; }        /* hit an end: stop and settle back */
        return true;
    }
    s->vel = 0;
    float target = clampf(roundf(s->pos), 0, hi);
    float k = SNAP_RATE * dt;
    s->pos += (target - s->pos) * (k > 1 ? 1 : k);
    if (fabsf(target - s->pos) < 0.003f) {
        s->pos = target;
        s->moving = false;
    }
    return s->moving;
}
