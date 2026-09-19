#include "pip.h"

#include "palette.h"

/* How fast the eyes chase the tilt. A first-order approach, alpha = dt*rate:
   at 9 it lands in a few tenths of a second -- alert without twitching on the
   accelerometer noise that a rigid follow would show. */
#define LOOK_RATE       9.0f
/* A startle fades over about this long. Long enough to read as a reaction,
   short enough that Pip is watching again by the time you have set it down. */
#define STARTLE_DECAY_S 0.9f
/* A blink every few seconds, briefly, and never mid-startle -- wide eyes do
   not blink. */
#define BLINK_EVERY_S   4.0f
#define BLINK_FOR_S     0.14f

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void pip_init(pip_t *p)
{
    p->look_x = p->look_y = 0.0f;
    p->startle = 0.0f;
    p->blink = 0.0f;
    p->since_blink_s = 0.0f;
    p->blink_left_s = 0.0f;
}

void pip_update(pip_t *p, float gx, float gy, bool shaken, float dt)
{
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.5f) dt = 0.5f;      /* a stall must not snap the eyes across */

    /* The eyes look the way "down" points, clamped so a hard tilt cannot
       throw the pupil out of the white. */
    float tx = clampf(gx, -1.0f, 1.0f);
    float ty = clampf(gy, -1.0f, 1.0f);
    float a = clampf(dt * LOOK_RATE, 0.0f, 1.0f);
    p->look_x += (tx - p->look_x) * a;
    p->look_y += (ty - p->look_y) * a;

    /* A shake pins startle to full; otherwise it bleeds away. */
    if (shaken) p->startle = 1.0f;
    else {
        p->startle -= dt / STARTLE_DECAY_S;
        if (p->startle < 0.0f) p->startle = 0.0f;
    }

    /* Blink on a slow clock, but not while startled. */
    p->since_blink_s += dt;
    if (p->blink_left_s > 0.0f) {
        p->blink_left_s -= dt;
        if (p->blink_left_s < 0.0f) p->blink_left_s = 0.0f;
    } else if (p->since_blink_s >= BLINK_EVERY_S && p->startle < 0.2f) {
        p->blink_left_s = BLINK_FOR_S;
        p->since_blink_s = 0.0f;
    }
    p->blink = p->blink_left_s > 0.0f ? 1.0f : 0.0f;
}

void pip_draw(canvas_t *c, const pip_t *p)
{
    canvas_clear(c);

    int w = c->w, h = c->h;
    int cx = w / 2;
    int eye_cy = h * 34 / 100;
    int sep = w * 22 / 100;

    /* Startle opens the eyes wider and pulls the pupils in -- the whites-all-
       round look of a small thing that just got a fright. */
    float wide = 1.0f + 0.28f * p->startle;
    int sclera_r = (int)((float)w * 0.17f * wide);
    if (sclera_r < 6) sclera_r = 6;
    int travel = (int)((float)sclera_r * 0.42f);
    int iris_r = (int)((float)sclera_r * 0.55f);
    int pupil_r = (int)((float)iris_r * 0.55f * (1.0f - 0.35f * p->startle));
    if (pupil_r < 1) pupil_r = 1;

    int ex[2] = { cx - sep, cx + sep };
    for (int i = 0; i < 2; i++) {
        int sx = ex[i], sy = eye_cy;
        if (p->blink > 0.5f) {
            /* Shut: a lid drawn as a bar where the eye was. */
            canvas_fill_rect(c, sx - sclera_r, sy - 2, 2 * sclera_r + 1, 4, PAL_FG);
            continue;
        }
        canvas_disc(c, sx, sy, sclera_r, PAL_FG);
        int px = sx + (int)(p->look_x * (float)travel);
        int py = sy + (int)(p->look_y * (float)travel);
        canvas_disc(c, px, py, iris_r, PAL_A2);
        canvas_disc(c, px, py, pupil_r, PAL_BG);
        /* A catchlight, so the eye looks wet rather than printed. */
        int gl = pupil_r / 3 > 0 ? pupil_r / 3 : 1;
        canvas_disc(c, px - pupil_r / 2, py - pupil_r / 2, gl, PAL_FG);
    }

    int my = h * 64 / 100;
    if (p->startle > 0.4f) {
        /* A surprised "o". */
        int r = (int)((float)w * 0.09f);
        canvas_disc(c, cx, my, r, PAL_FG);
        canvas_disc(c, cx, my, r - 3 > 0 ? r - 3 : 1, PAL_BG);
    } else {
        /* A small smile: a bar with its ends turned up. */
        int half = w * 16 / 100;
        int th = 4;
        canvas_fill_rect(c, cx - half, my, 2 * half, th, PAL_FG);
        canvas_fill_rect(c, cx - half, my - 4, th, 6, PAL_FG);
        canvas_fill_rect(c, cx + half - th, my - 4, th, 6, PAL_FG);
    }
}
