#include "pip.h"

#include <stdio.h>
#include <string.h>

static int failures;
static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

/* A few seconds of steady tilt, so the smoothed eyes reach the target. */
static void hold(pip_t *p, float gx, float gy, float secs)
{
    for (float t = 0; t < secs; t += 0.05f)
        pip_update(p, gx, gy, false, 0.05f);
}

/* ---- the eyes follow gravity ---------------------------------------- */

static void test_look(void)
{
    pip_t p;
    pip_init(&p);
    expect("starts looking straight ahead", p.look_x == 0.0f && p.look_y == 0.0f);

    hold(&p, 0.9f, 0.0f, 1.5f);
    expect("tilt right -> eyes look right", p.look_x > 0.3f);
    expect("no vertical drift from a horizontal tilt", p.look_y > -0.1f && p.look_y < 0.1f);

    hold(&p, -0.9f, 0.0f, 1.5f);
    expect("tilt left -> eyes look left", p.look_x < -0.3f);

    hold(&p, 0.0f, 0.9f, 1.5f);
    expect("tilt down -> eyes look down", p.look_y > 0.3f);

    /* A hard tilt must not throw the pupil out of the eye. */
    hold(&p, 5.0f, -5.0f, 1.5f);
    expect("look stays within the eye (x)", p.look_x <= 1.0f && p.look_x >= -1.0f);
    expect("look stays within the eye (y)", p.look_y <= 1.0f && p.look_y >= -1.0f);
}

/* ---- shaking startles, then it settles ------------------------------ */

static void test_startle(void)
{
    pip_t p;
    pip_init(&p);
    expect("starts calm", p.startle == 0.0f);

    pip_update(&p, 0, 0, true, 0.05f);
    expect("a shake startles it", p.startle > 0.9f);

    /* With no more shakes it calms down within a second or so. */
    for (float t = 0; t < 1.5f; t += 0.05f)
        pip_update(&p, 0, 0, false, 0.05f);
    expect("it settles after a shake", p.startle < 0.1f);

    /* Startle never runs negative however long it is left. */
    for (int i = 0; i < 200; i++) pip_update(&p, 0, 0, false, 0.05f);
    expect("startle floors at zero", p.startle >= 0.0f);
}

/* ---- it draws something, without running off its buffer -------------- */

#define W 120
#define H 180
static uint16_t guarded[8 + W * H + 8];

static long non_bg(void)
{
    long n = 0;
    for (int i = 0; i < W * H; i++)
        if (guarded[8 + i] != CANVAS_BG) n++;
    return n;
}

static void test_draw(void)
{
    for (unsigned i = 0; i < sizeof guarded / sizeof guarded[0]; i++)
        guarded[i] = 0xABAB;
    canvas_t c;
    canvas_init(&c, guarded + 8, W, H, 1);

    pip_t p;
    pip_init(&p);
    pip_update(&p, 0.0f, 0.0f, false, 0.05f);
    pip_draw(&c, &p);

    int guards_ok = 1;
    for (int i = 0; i < 8; i++)
        if (guarded[i] != 0xABAB || guarded[8 + W * H + i] != 0xABAB) guards_ok = 0;
    expect("draw stays inside the framebuffer", guards_ok);
    expect("a face is drawn", non_bg() > 100);
}

int main(void)
{
    test_look();
    test_startle();
    test_draw();
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
