#include "tiltgame.h"

#include <stdio.h>

static int failures;
static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

#define W 240
#define H 135

static void run(tiltgame_t *g, float gx, float gy, int frames)
{
    for (int i = 0; i < frames; i++) tiltgame_step(g, gx, gy, 1.0f / 30.0f);
}

static int inside_a_wall(const tiltgame_t *g)
{
    for (int i = 0; i < TG_WALLS; i++) {
        const tg_rect_t *r = &g->walls[i];
        /* Allow a pixel of slack: the ball is pushed to the face, not past. */
        if (g->bx > (float)r->x + 1.0f && g->bx < (float)(r->x + r->w) - 1.0f
         && g->by > (float)r->y + 1.0f && g->by < (float)(r->y + r->h) - 1.0f)
            return 1;
    }
    return 0;
}

int main(void)
{
    tiltgame_t g;

    tiltgame_init(&g, W, H, 12345u);
    expect("starts with nothing caught", g.score == 0);
    expect("starts with a full clock", g.seconds_left == TG_START_SECONDS);
    expect("starts running", !g.over);

    /* The ball answers the board, and stays on the panel doing it. */
    float x0 = g.bx;
    run(&g, 700.0f, 0.0f, 20);
    expect("tilting right rolls the ball right", g.bx > x0 + 5.0f);

    int escaped = 0, embedded = 0;
    const float dirs[4][2] = { {900,0}, {-900,0}, {0,900}, {0,-900} };
    for (int d = 0; d < 4; d++) {
        tiltgame_init(&g, W, H, 7u + (unsigned)d);
        for (int i = 0; i < 600; i++) {
            tiltgame_step(&g, dirs[d][0], dirs[d][1], 1.0f / 30.0f);
            if (g.bx < 0 || g.bx > W || g.by < 0 || g.by > H) escaped++;
            if (inside_a_wall(&g)) embedded++;
        }
    }
    expect("the ball never leaves the arena", escaped == 0);
    expect("and never ends up inside a wall", embedded == 0);

    /* Catching a ring scores and buys time. */
    tiltgame_init(&g, W, H, 99u);
    g.bx = (float)g.tx;
    g.by = (float)g.ty;
    float before = g.seconds_left;
    tiltgame_step(&g, 0.0f, 0.0f, 1.0f / 30.0f);
    expect("rolling onto the ring scores", g.score == 1);
    expect("and adds time", g.seconds_left > before);
    expect("and the ring moves elsewhere",
           !(g.bx == (float)g.tx && g.by == (float)g.ty));
    expect("the best score follows the score", g.best == 1);

    /* The clock runs out, and a new round starts by itself. */
    tiltgame_init(&g, W, H, 4u);
    run(&g, 0.0f, 0.0f, 30 * 30);
    expect("time runs out", g.over);
    int best_then = g.best;
    /* Time on the over screen counts toward the restart, and by now some of
       it has already passed, so the new round is a few seconds old when this
       looks at it. What matters is that it is running and starts from
       nothing, not exactly how much clock is left. */
    run(&g, 0.0f, 0.0f, (int)(30.0f * (TG_RESTART_AFTER + 1.0f)));
    expect("a new round starts on its own", !g.over);
    expect("from nothing caught", g.score == 0);
    expect("with a clock that is running", g.seconds_left > 0.0f
           && g.seconds_left <= TG_START_SECONDS);
    expect("the best score survives it", g.best == best_then);

    /* Drawing, in both states, stays inside the framebuffer. */
    static uint16_t guarded[8 + W * H + 8];
    for (unsigned i = 0; i < sizeof guarded / sizeof guarded[0]; i++)
        guarded[i] = 0xABAB;
    canvas_t c;
    canvas_init(&c, guarded + 8, W, H, 2);
    tiltgame_init(&g, W, H, 3u);
    tiltgame_draw(&g, &c);
    g.over = true;
    tiltgame_draw(&g, &c);
    int intact = 1;
    for (int i = 0; i < 8; i++)
        if (guarded[i] != 0xABAB || guarded[8 + W * H + i] != 0xABAB) intact = 0;
    expect("drawing stays in the framebuffer", intact);

    printf("%s\n", failures ? "FAILURES" : "all tests passed");
    return failures ? 1 : 0;
}
