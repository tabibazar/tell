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

/* Every grain on a real pixel: [0,w) x [0,h). A grain at exactly w or h is
   off the panel, because its block is drawn from there down and right. */
static int on_panel(const particles_t *s)
{
    for (int i = 0; i < s->n; i++)
        if (s->p[i].x < 0.0f || s->p[i].x >= (float)s->w
         || s->p[i].y < 0.0f || s->p[i].y >= (float)s->h)
            return 0;
    return 1;
}

/* One display frame of `dt`, taken the way main.c takes it: a single
   particles_step call, which cuts it into steps itself. Returns 0 if a grain
   ended the frame off the panel. */
static int frame(particles_t *s, float gx, float gy, float dt)
{
    particles_step(s, gx, gy, dt);
    return on_panel(s);
}

/* Where each grain was when the watched frame began. */
static float fx[PARTICLES_MAX], fy[PARTICLES_MAX];

/* Pours for 150 frames, then watches one more second. How far a grain moves
   over a whole frame is what the eye sees as shimmer, so both measures are
   taken frame to frame: the worst single move, and the mean square of every
   grain's move over the frame's time, as a speed. Not the grains' own
   velocity: that is the last step's, and a frame cut into three steps reads
   a tenth of a pixel of settling as three times the speed the eye sees.
   Returns 0 if a grain was ever off the panel. */
static int pour_and_watch(particles_t *s, float gx, float gy, float dt,
                          double *speed2, float *worst2)
{
    int ok = 1;
    for (int f = 0; f < 150; f++)
        if (!frame(s, gx, gy, dt)) ok = 0;

    *speed2 = 0;
    *worst2 = 0;
    for (int f = 0; f < 30; f++) {
        for (int i = 0; i < s->n; i++) { fx[i] = s->p[i].x; fy[i] = s->p[i].y; }
        if (!frame(s, gx, gy, dt)) ok = 0;
        double moved2 = 0;
        for (int i = 0; i < s->n; i++) {
            float dx = s->p[i].x - fx[i], dy = s->p[i].y - fy[i];
            moved2 += dx * dx + dy * dy;
            if (dx * dx + dy * dy > *worst2) *worst2 = dx * dx + dy * dy;
        }
        *speed2 += moved2 / (double)s->n / ((double)dt * (double)dt);
    }
    *speed2 /= 30.0;
    return ok;
}

/* Below 25 px/s and 8 px a frame. A settled pile moves no grain more than a
   few pixels a frame; shimmering, as watch did at 1200 in one step a frame,
   she moved grains 24 px a frame. */
static int at_rest(double speed2, float worst2)
{
    return speed2 < 25.0 * 25.0 && worst2 < 8.0f * 8.0f;
}

int main(void)
{
    particles_t s;

    /* Bounds, under gravity pointing each of four ways. */
    const float g[4][2] = { {0, 400}, {0, -400}, {400, 0}, {-400, 0} };
    for (int d = 0; d < 4; d++) {
        particles_init(&s, 200, W, H, 12345u + (unsigned)d);
        run(&s, g[d][0], g[d][1], 1000);
        expect("particles stay on the panel", on_panel(&s));
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

    /*
     * lilly's panel, as wide as any the grid is sized for. The bucket grid
     * is exactly as wide as this, so a grain at the far corner must still land in a cell
     * that exists: if the grid were short the clamp in particles_init would
     * quietly fold the right-hand columns together and grains there would
     * stop separating.
     */
#define LW 320
#define LH 170
    static particles_t lilly;
    particles_init(&lilly, particles_for(LW, LH), LW, LH, 77u);
    expect("lilly's grid covers her panel", lilly.gw == LW / PARTICLES_CELL + 1
                                         && lilly.gh == LH / PARTICLES_CELL + 1);
    expect("lilly's grid fits the arrays",
           lilly.gw <= PARTICLES_GRID_W && lilly.gh <= PARTICLES_GRID_H);

    /* Same grains per pixel on either panel, so the bottle looks equally full
       whichever one it is drawn on. */
    expect("lilly gets 320 grains", particles_for(LW, LH) == 320);
    expect("the Feather's panel still gets 190", particles_for(W, H) == 190);
    expect("a huge panel is clamped", particles_for(4000, 4000) == PARTICLES_MAX);

    /* wave's panel, which is the one that actually has the IMU. Two pixel
       rows taller than lilly, so a few more grains, and the bucket grid
       still covers it -- 172/10+1 is 18. */
    expect("wave gets 323 grains", particles_for(320, 172) == 323);
    expect("wave's grid still fits", 320 / PARTICLES_CELL + 1 <= PARTICLES_GRID_W
                                  && 172 / PARTICLES_CELL + 1 <= PARTICLES_GRID_H);

    /*
     * The physics must hold at this size AND at the gravity this size is
     * actually driven at. main.c scales gravity per panel row, so lilly gets
     * GRAVITY_PER_ROW * 170, about 1134 -- a quarter more than the 900 the
     * Feather's numbers were tuned at. That is the thing resizing could
     * break: the grains are no bigger, so each one moves further relative to
     * its own radius per step, and the pairwise separation that holds a
     * column up has to keep pace. Testing her at 900 would prove nothing.
     *
     * The floor is where a grain's block still fits, not h. It used to be h,
     * which put the whole settled bottom layer -- 74 of her 320 grains --
     * just past the last row, where drawing clipped them away entirely.
     */
#define LILLY_GRAVITY 1134.0f
    run(&lilly, 0, LILLY_GRAVITY, 300);
    expect("grains stay within lilly's panel", on_panel(&lilly));
    expect("and pour to the bottom of it", centre_y(&lilly) > (float)LH / 2.0f);

    /* The column still stands at her gravity. 320 grains of radius 5 across
       320 px is about ten rows if they hold each other up, and two or three
       if they do not -- so this is the assertion that says the separation
       survived the stronger pull, not merely that the grains fell. */
    {
        int rows[LH + 1];
        for (int i = 0; i <= LH; i++) rows[i] = 0;
        for (int i = 0; i < lilly.n; i++) {
            int r = (int)lilly.p[i].y;
            if (r >= 0 && r <= LH) rows[r]++;
        }
        int occupied = 0;
        for (int i = 0; i <= LH; i++) if (rows[i] > 0) occupied++;
        printf("     (lilly's pile occupies %d rows at g=%.0f)\n",
               occupied, (double)LILLY_GRAVITY);
        expect("the pile still stands a real column at her gravity",
               occupied > 6);
    }

    /* The same out-of-bounds guard at her size, where the grid is exactly the
       panel rather than the Feather's comfortable margin. */
    static uint16_t lguard[8 + LW * LH + 8];
    memset(lguard, 0xAB, sizeof lguard);
    canvas_t lc;
    canvas_init(&lc, lguard + 8, LW, LH, 1);
    particles_draw(&lilly, &lc);
    int lilly_guards = 1;
    for (int i = 0; i < 8; i++)
        if (lguard[i] != 0xABAB || lguard[8 + LW * LH + i] != 0xABAB)
            lilly_guards = 0;
    expect("draw stays inside lilly's framebuffer", lilly_guards);

    /*
     * watch: the Touch-LCD-1.69, 240x280 portrait. The first panel taller
     * than it is wide, and the first the old 33x18 grid could not hold: 280
     * rows want 29 rows of cells, and the clamp folded everything below
     * y = 170 into one, so the bottom 110 px of the pile -- where it is
     * deepest and most needs to separate -- was one row of buckets.
     */
#define WW 240
#define WH 280
    static particles_t watch;
    particles_init(&watch, particles_for(WW, WH), WW, WH, 2024u);
    expect("watch gets 395 grains", watch.n == 395);
    expect("watch's grid covers her panel without clamping",
           watch.gw == WW / PARTICLES_CELL + 1 && watch.gh == WH / PARTICLES_CELL + 1);
    expect("watch's grid fits the arrays",
           watch.gw <= PARTICLES_GRID_W && watch.gh <= PARTICLES_GRID_H);
    printf("     (a particles_t is %zu bytes)\n", sizeof(particles_t));

    /* Bounds hold at any gravity, not only one that settles: poured four
       ways at the 1867 that main.c's 6.67 per row would ask for on 280 rows
       if nothing capped it, shaken every half second at her shake ceiling
       (1.48 per row, 414 px/s), and checked after every step. */
#define WATCH_UNCAPPED 1867.0f
    {
        const float wg[4][2] = { {0, WATCH_UNCAPPED}, {0, -WATCH_UNCAPPED},
                                 {WATCH_UNCAPPED, 0}, {-WATCH_UNCAPPED, 0} };
        int always = 1;
        for (int d = 0; d < 4; d++) {
            particles_init(&watch, particles_for(WW, WH), WW, WH, 600u + (unsigned)d);
            for (int f = 0; f < 300; f++) {
                if (f % 15 == 0) particles_agitate(&watch, 414.0f);
                if (!frame(&watch, wg[d][0], wg[d][1], 1.0f / 30.0f)) always = 0;
            }
        }
        expect("no grain ever leaves watch's panel, poured and shaken four ways",
               always);
    }

    /*
     * How many steps a frame is cut into. This is the sand's cost per
     * frame -- each step is a whole solve -- so it is pinned here rather
     * than left to fall out of the constants. Lying flat, gravity in the
     * panel's plane is nearly nothing and one step does; at the cap it is
     * two at 30 fps and three once the frame runs past about 36 ms; a
     * stall is capped at three rather than caught up.
     */
    expect("lying flat, one step a frame", particles_substeps(0, 0, 1.0f / 30.0f) == 1);
    expect("at the cap at 30 fps, two steps",
           particles_substeps(0, PARTICLES_GRAVITY_MAX, 1.0f / 30.0f) == 2);
    expect("tipped the cap is the same two",
           particles_substeps(849.0f, 849.0f, 1.0f / 30.0f) == 2);
    expect("at the cap at 25 fps, three",
           particles_substeps(0, PARTICLES_GRAVITY_MAX, 0.040f) == 3);
    expect("uncapped on watch at 30 fps, three",
           particles_substeps(0, WATCH_UNCAPPED, 1.0f / 30.0f) == 3);
    expect("a stall is capped at three, not caught up",
           particles_substeps(0, PARTICLES_GRAVITY_MAX, 0.2f) == 3);
    expect("no time, one step", particles_substeps(0, PARTICLES_GRAVITY_MAX, 0.0f) == 1);

    /*
     * And the pile settles, driven exactly the way main.c drives it: one
     * particles_step call a frame at the capped gravity, pointing whichever
     * way the board is held. The solver brings a pile to rest only if no
     * step sinks it further than the separation passes can hold it up;
     * support climbs from the floor about a layer a pass, and watch's pile
     * is the deepest here -- 395 grains across 240, and about twenty layers
     * wedged into a corner. Taken as one step a frame, at the 1200 main.c
     * caps her at, she shimmered without end: 75 px/s, grains jumping 24 px
     * a frame. That first row is the regression.
     *
     * Also covered: the full 1867 her 280 rows would ask for uncapped, and
     * slower frames, since main.c passes the real frame time and a 30 fps
     * page lands anywhere from 30 to 40 ms on a 10 ms tick.
     */
    {
        static const struct { const char *how; float gx, gy, dt; } ways[] = {
            { "one call a frame at the cap",           0, PARTICLES_GRAVITY_MAX, 1.0f / 30.0f },
            { "tipped corner-down at the cap",     849.0f, 849.0f,                1.0f / 30.0f },
            { "on her side at the cap", PARTICLES_GRAVITY_MAX, 0,                 1.0f / 30.0f },
            { "at the cap at 25 fps",                  0, PARTICLES_GRAVITY_MAX, 0.040f },
            { "tipped at the cap at 20 fps",       849.0f, 849.0f,                0.050f },
            { "uncapped at 30 fps",                    0, WATCH_UNCAPPED,        1.0f / 30.0f },
        };
        char what[128];

        for (size_t k = 0; k < sizeof ways / sizeof ways[0]; k++) {
            particles_init(&watch, particles_for(WW, WH), WW, WH, 2024u);
            float start = centre_y(&watch);
            double speed2;
            float worst2;
            int always = pour_and_watch(&watch, ways[k].gx, ways[k].gy, ways[k].dt,
                                        &speed2, &worst2);

            /* How full the bottle looks, when it is poured straight down: the
               pile's height over the panel's. The surface is the row above
               which only 3% of the grains lie, so one straggler in flight
               does not count as the top. Close packed, 395 grains of radius
               5 would fill half of it; the square-law push lets them squash
               a little. She stands at about 0.43, lilly and wave at 0.48;
               taken in one step a frame, before the solver cut its steps,
               the squash was worse and they stood at 0.29 and 0.37. A pile
               that never fell reads near 1, one that collapsed into a few
               rows near 0.

               The row index is guarded rather than trusted: this is the
               block that catches a grain past the floor, and it must say
               FAIL when it does, not write past the end of rows[]. */
            int rows[WH];
            for (int r = 0; r < WH; r++) rows[r] = 0;
            float lowest = 0;
            for (int i = 0; i < watch.n; i++) {
                int r = (int)watch.p[i].y;
                if (r >= 0 && r < WH) rows[r]++;
                if (watch.p[i].y > lowest) lowest = watch.p[i].y;
            }
            int above = 0, top = 0, occupied = 0;
            for (int r = 0; r < WH; r++) if (rows[r] > 0) occupied++;
            for (int r = 0; r < WH; r++) {
                above += rows[r];
                if (above > watch.n * 3 / 100) { top = r; break; }
            }
            float fill = (float)(WH - top) / (float)WH;

            printf("     (watch, %s: %d steps, fill %.2f, %d rows, mean square "
                   "speed %.0f, worst move %.1f px^2)\n", ways[k].how,
                   particles_substeps(ways[k].gx, ways[k].gy, ways[k].dt),
                   (double)fill, occupied, speed2, (double)worst2);

            snprintf(what, sizeof what, "watch, %s: never off the panel", ways[k].how);
            expect(what, always);
            if (ways[k].gx == 0) {
                /* The floor stops a grain's block short of the last row, so
                   the lowest grains rest a few pixels up rather than on WH
                   itself. */
                snprintf(what, sizeof what, "watch, %s: pours to the floor", ways[k].how);
                expect(what, centre_y(&watch) > start + 40.0f
                          && centre_y(&watch) > (float)WH * 2.0f / 3.0f
                          && lowest > (float)WH - 2.0f * PARTICLES_RADIUS);
                snprintf(what, sizeof what, "watch, %s: fills a sane share of the bottle",
                         ways[k].how);
                expect(what, fill > 0.2f && fill < 0.6f && occupied > 10);
            }
            snprintf(what, sizeof what, "watch, %s: comes to rest", ways[k].how);
            expect(what, at_rest(speed2, worst2));
        }
    }

    /*
     * The same failure was on lilly and wave all along, just not lying flat:
     * on her side or tipped corner-down, lilly's pile is as deep as watch's
     * and shimmered at 94 px/s in one step a frame at her own gravity. wave
     * stood on her end likewise, at 47. Cut into steps, both come to rest.
     */
    {
        static const struct { const char *how; int w, h; float gx, gy; } others[] = {
            { "lilly on her side",          LW,  LH, LILLY_GRAVITY, 0 },
            { "lilly tipped corner-down",   LW,  LH, 802.0f, 802.0f },
            { "wave stood on her end",     172, 320, 0, 1147.0f },
        };
        static particles_t other;
        char what[128];

        for (size_t k = 0; k < sizeof others / sizeof others[0]; k++) {
            particles_init(&other, particles_for(others[k].w, others[k].h),
                           others[k].w, others[k].h, 77u);
            double speed2;
            float worst2;
            int always = pour_and_watch(&other, others[k].gx, others[k].gy,
                                        1.0f / 30.0f, &speed2, &worst2);
            printf("     (%s: mean square speed %.0f, worst move %.1f px^2)\n",
                   others[k].how, speed2, (double)worst2);
            snprintf(what, sizeof what, "%s: never off the panel", others[k].how);
            expect(what, always);
            snprintf(what, sizeof what, "%s: comes to rest", others[k].how);
            expect(what, at_rest(speed2, worst2));
        }
    }

    /* Drawing stays inside her framebuffer too, with the pile settled on
       the last row. */
    static uint16_t wguard[8 + WW * WH + 8];
    memset(wguard, 0xAB, sizeof wguard);
    canvas_t wc;
    canvas_init(&wc, wguard + 8, WW, WH, 1);
    particles_draw(&watch, &wc);
    int watch_guards = 1;
    for (int i = 0; i < 8; i++)
        if (wguard[i] != 0xABAB || wguard[8 + WW * WH + i] != 0xABAB)
            watch_guards = 0;
    expect("draw stays inside watch's framebuffer", watch_guards);

    printf("%s\n", failures ? "FAILURES" : "all tests passed");
    return failures ? 1 : 0;
}
