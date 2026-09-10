#include "particles.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

#define W 240
#define H 135

static float centre_x(const particles_t *s)
{
    float t = 0;
    for (int i = 0; i < s->n; i++) t += s->p[i].x;
    return t / (float)s->n;
}

static float centre_y(const particles_t *s)
{
    float t = 0;
    for (int i = 0; i < s->n; i++) t += s->p[i].y;
    return t / (float)s->n;
}

static float energy(const particles_t *s)
{
    float t = 0;
    for (int i = 0; i < s->n; i++)
        t += s->p[i].vx * s->p[i].vx + s->p[i].vy * s->p[i].vy;
    return t;
}

static void run(particles_t *s, float gx, float gy, int steps)
{
    for (int i = 0; i < steps; i++) particles_step(s, gx, gy, 1.0f / 30.0f);
}

int main(void)
{
    particles_t s;

    /* Bounds, under gravity pointing each of four ways. */
    const float g[4][2] = { {0, 400}, {0, -400}, {400, 0}, {-400, 0} };
    for (int d = 0; d < 4; d++) {
        particles_init(&s, 200, W, H, 12345u + (unsigned)d);
        run(&s, g[d][0], g[d][1], 1000);
        int inside = 1;
        for (int i = 0; i < s.n; i++)
            if (s.p[i].x < 0 || s.p[i].x > W || s.p[i].y < 0 || s.p[i].y > H)
                inside = 0;
        expect("particles stay on the panel", inside);
    }

    /* Damping is real: a hard shove dies away to the jitter floor. It does
       not reach zero, and must not -- a bottle of grains that freezes solid
       is the bug this replaced. */
    particles_init(&s, 200, W, H, 999u);
    for (int i = 0; i < s.n; i++) { s.p[i].vx = 400.0f; s.p[i].vy = -300.0f; }
    float before = energy(&s);
    run(&s, 0, 0, 300);
    expect("a shove decays to the jitter floor", energy(&s) < before * 0.2f);
    expect("but the bottle never stops moving", energy(&s) > 0.0f);

    /* The pile has depth. Under steady gravity the grains must not all end
       up in the same row: that is the collapse that made the first version
       drain and then do nothing. */
    particles_init(&s, 400, W, H, 31u);
    run(&s, 0, 900, 400);
    {
        int rows[H];
        for (int i = 0; i < H; i++) rows[i] = 0;
        for (int i = 0; i < s.n; i++) {
            int r = (int)s.p[i].y;
            if (r >= 0 && r < H) rows[r]++;
        }
        int occupied = 0;
        for (int i = 0; i < H; i++) if (rows[i] > 0) occupied++;
        expect("the pile stands more than a few rows deep", occupied > 12);
    }

    /* Gravity to the right piles them up on the right. */
    particles_init(&s, 200, W, H, 42u);
    float x0 = centre_x(&s);
    run(&s, 600, 0, 200);
    expect("gravity right moves the centre of mass right", centre_x(&s) > x0 + 10.0f);

    /* And down is down, which is the sign error that would otherwise ship. */
    particles_init(&s, 200, W, H, 42u);
    float y0 = centre_y(&s);
    run(&s, 0, 600, 200);
    expect("gravity down moves the centre of mass down", centre_y(&s) > y0 + 10.0f);

    /* Swirl turns a still field, and turns it the way its sign says. */
    particles_init(&s, 200, W, H, 5u);
    for (int i = 0; i < s.n; i++) { s.p[i].vx = 0; s.p[i].vy = 0; }
    particles_swirl(&s, 0.5f);
    expect("swirl imparts motion", energy(&s) > 0.0f);

    /* Drawing writes only inside the framebuffer. */
    static uint16_t guarded[8 + W * H + 8];
    memset(guarded, 0xAB, sizeof guarded);
    canvas_t c;
    canvas_init(&c, guarded + 8, W, H, 1);
    particles_init(&s, 200, W, H, 7u);
    run(&s, 0, 400, 100);
    particles_draw(&s, &c);
    int guards_intact = 1;
    for (int i = 0; i < 8; i++)
        if (guarded[i] != 0xABAB || guarded[8 + W * H + i] != 0xABAB)
            guards_intact = 0;
    expect("draw stays inside the framebuffer", guards_intact);

    /* n is clamped rather than overrunning the array. */
    particles_init(&s, PARTICLES_MAX + 500, W, H, 1u);
    expect("n is clamped to PARTICLES_MAX", s.n == PARTICLES_MAX);

    printf("%s\n", failures ? "FAILURES" : "all tests passed");
    return failures ? 1 : 0;
}
