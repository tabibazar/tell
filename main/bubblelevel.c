#include "bubblelevel.h"

#include "palette.h"

#include <math.h>
#include <stdio.h>

/* Px the bubble travels at full tilt. */
#define BUBBLE_RANGE 130.0f
/* Per-second lerp toward the target -- it floats, not snaps. */
#define BUBBLE_SMOOTH 8.0f
/* Seconds held within tolerance to advance a level. */
#define BUBBLE_HOLD_WIN 2.0f

/* Tolerance radius in px, one per level, tightening. */
static const float tol[BUBBLE_LEVELS] = { 34, 22, 13 };

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void bubble_init(bubble_t *b)
{
    b->bx = b->by = 0.0f;
    b->held_s = 0.0f;
    b->level = 0;
    b->won = false;
    b->roll_deg = b->pitch_deg = 0.0f;
}

void bubble_reset(bubble_t *b)
{
    b->held_s = 0.0f;
}

void bubble_update(bubble_t *b, float gx, float gy, float dt)
{
    if (dt < 0.0f) dt = 0.0f;

    /* A real spirit level's bubble rises to the HIGH side -- opposite
       gravity. */
    float tx = -gx * BUBBLE_RANGE;
    float ty = -gy * BUBBLE_RANGE;
    float d = hypotf(tx, ty);
    if (d > BUBBLE_RANGE) {
        float s = BUBBLE_RANGE / d;
        tx *= s;
        ty *= s;
    }

    float a = fminf(BUBBLE_SMOOTH * dt, 1.0f);
    b->bx += (tx - b->bx) * a;
    b->by += (ty - b->by) * a;

    b->roll_deg = asinf(fmaxf(-1.0f, fminf(1.0f, gx))) * 57.2958f;
    b->pitch_deg = asinf(fmaxf(-1.0f, fminf(1.0f, gy))) * 57.2958f;

    if (b->won) return;

    float dist = hypotf(b->bx, b->by);
    if (dist < tol[b->level]) {
        b->held_s += dt;
        if (b->held_s >= BUBBLE_HOLD_WIN) {
            if (b->level + 1 < BUBBLE_LEVELS) {
                b->level++;
                b->held_s = 0.0f;
            } else {
                b->won = true;
            }
        }
    } else {
        b->held_s = 0.0f;
    }
}

void bubble_draw(canvas_t *c, const bubble_t *b)
{
    canvas_clear(c);

    int cx = c->w / 2, cy = c->h / 2;

    /* Target rings: every tolerance in blue, the current level's ring in
       amber so the one you must hold stands out. */
    for (int i = 0; i < BUBBLE_LEVELS; i++) {
        canvas_circle(c, cx, cy, (int)tol[i],
                       i == b->level ? PAL_A1 : PAL_A0);
    }

    /* Hold progress: the current ring redrawn thicker, growing toward the
       full tolerance as held_s approaches BUBBLE_HOLD_WIN. */
    float frac = clampf(b->held_s / BUBBLE_HOLD_WIN, 0.0f, 1.0f);
    if (frac > 0.0f) {
        int rr = (int)(tol[b->level] * frac);
        canvas_circle(c, cx, cy, rr, PAL_A1);
    }

    /* Centre crosshair. */
    canvas_fill_rect(c, cx - 6, cy, 13, 1, PAL_FG);
    canvas_fill_rect(c, cx, cy - 6, 1, 13, PAL_FG);

    /* The bubble itself. */
    int bx = cx + (int)b->bx, by = cy + (int)b->by;
    canvas_disc(c, bx, by, 12, PAL_A1);

    char buf[32];
    if (b->won) {
        snprintf(buf, sizeof buf, "LEVEL MASTER");
    } else {
        snprintf(buf, sizeof buf, "Level %d/%d", b->level + 1, BUBBLE_LEVELS);
    }
    canvas_puts_px(c, 4, 4, buf, PAL_FG);

    snprintf(buf, sizeof buf, "R %+.0f  P %+.0f",
             (double)b->roll_deg, (double)b->pitch_deg);
    canvas_puts_px(c, 4, 4 + c->cell_h, buf, PAL_FG);
}
