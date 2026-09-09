#include "tiltgame.h"

#include "palette.h"

#include <stdio.h>

/* The ball keeps rolling, but not for ever. */
#define DRAG 0.65f
/* A wall gives back almost nothing, so the ball does not ping around. */
#define BOUNCE 0.35f
/* The top of the panel belongs to the clock bar and the score. */
#define HEADER_H 22

static uint32_t next_rand(tiltgame_t *g)
{
    g->rng = g->rng * 1103515245u + 12345u;
    return g->rng >> 16;
}

static int rand_between(tiltgame_t *g, int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)(next_rand(g) % (uint32_t)(hi - lo));
}

static bool near_rect(const tg_rect_t *r, int x, int y, int pad)
{
    return x >= r->x - pad && x <= r->x + r->w + pad
        && y >= r->y - pad && y <= r->y + r->h + pad;
}

/* Somewhere clear of every wall, and not on top of the ball -- a ring that
   appears under the ball would be caught before it was seen. */
static void place_target(tiltgame_t *g)
{
    for (int tries = 0; tries < 60; tries++) {
        int x = rand_between(g, TG_TARGET_R + 2, g->w - TG_TARGET_R - 2);
        int y = rand_between(g, HEADER_H + TG_TARGET_R, g->h - TG_TARGET_R - 2);
        bool clear = true;
        for (int i = 0; i < TG_WALLS; i++)
            if (near_rect(&g->walls[i], x, y, TG_TARGET_R + 3)) clear = false;
        float dx = (float)x - g->bx, dy = (float)y - g->by;
        if (dx * dx + dy * dy < 40.0f * 40.0f) clear = false;
        if (clear) { g->tx = x; g->ty = y; return; }
    }
    /* Sixty tries without a clear spot means the walls fell badly. The
       middle is always reachable, because no wall is placed across it. */
    g->tx = g->w / 2;
    g->ty = (g->h + HEADER_H) / 2;
}

static void place_walls(tiltgame_t *g)
{
    for (int i = 0; i < TG_WALLS; i++) {
        bool tall = (next_rand(g) & 1u) != 0;
        int len = rand_between(g, 24, 52);
        g->walls[i].w = tall ? 5 : len;
        g->walls[i].h = tall ? len : 5;
        g->walls[i].x = rand_between(g, 14, g->w - g->walls[i].w - 14);
        g->walls[i].y = rand_between(g, HEADER_H + 6,
                                     g->h - g->walls[i].h - 14);
    }
}

void tiltgame_restart(tiltgame_t *g)
{
    g->bx = (float)g->w / 2.0f;
    g->by = (float)(g->h + HEADER_H) / 2.0f;
    g->vx = g->vy = 0.0f;
    g->score = 0;
    g->seconds_left = TG_START_SECONDS;
    g->over = false;
    g->over_for = 0.0f;
    place_walls(g);
    place_target(g);
}

void tiltgame_init(tiltgame_t *g, int w, int h, uint32_t seed)
{
    g->w = w;
    g->h = h;
    g->rng = seed ? seed : 1u;
    g->best = 0;
    tiltgame_restart(g);
}

/* Pushes the ball out of a wall along whichever side it is least far into --
   which is the side it came in by -- and reverses its velocity on that axis
   alone, so a glancing blow does not stop it dead. */
static void unwall(tiltgame_t *g, const tg_rect_t *r)
{
    float left   = g->bx + (float)TG_BALL_R - (float)r->x;
    float right  = (float)(r->x + r->w) - (g->bx - (float)TG_BALL_R);
    float top    = g->by + (float)TG_BALL_R - (float)r->y;
    float bottom = (float)(r->y + r->h) - (g->by - (float)TG_BALL_R);
    if (left <= 0.0f || right <= 0.0f || top <= 0.0f || bottom <= 0.0f) return;

    float least = left; int side = 0;
    if (right < least)  { least = right;  side = 1; }
    if (top < least)    { least = top;    side = 2; }
    if (bottom < least) { least = bottom; side = 3; }

    switch (side) {
    case 0:  g->bx -= least; g->vx = -g->vx * BOUNCE; break;
    case 1:  g->bx += least; g->vx = -g->vx * BOUNCE; break;
    case 2:  g->by -= least; g->vy = -g->vy * BOUNCE; break;
    default: g->by += least; g->vy = -g->vy * BOUNCE; break;
    }
}

void tiltgame_step(tiltgame_t *g, float gx, float gy, float dt)
{
    if (g->over) {
        g->over_for += dt;
        if (g->over_for >= TG_RESTART_AFTER) tiltgame_restart(g);
        return;
    }

    float damp = 1.0f - (1.0f - DRAG) * dt;
    if (damp < 0.0f) damp = 0.0f;

    g->vx = (g->vx + gx * dt) * damp;
    g->vy = (g->vy + gy * dt) * damp;
    g->bx += g->vx * dt;
    g->by += g->vy * dt;

    float r = (float)TG_BALL_R;
    if (g->bx < r) { g->bx = r; g->vx = -g->vx * BOUNCE; }
    if (g->by < (float)HEADER_H + r) {
        g->by = (float)HEADER_H + r; g->vy = -g->vy * BOUNCE;
    }
    if (g->bx > (float)g->w - r) { g->bx = (float)g->w - r; g->vx = -g->vx * BOUNCE; }
    if (g->by > (float)g->h - r) { g->by = (float)g->h - r; g->vy = -g->vy * BOUNCE; }

    for (int i = 0; i < TG_WALLS; i++) unwall(g, &g->walls[i]);

    float dx = g->bx - (float)g->tx, dy = g->by - (float)g->ty;
    float reach = (float)(TG_BALL_R + TG_TARGET_R);
    if (dx * dx + dy * dy <= reach * reach) {
        g->score++;
        if (g->score > g->best) g->best = g->score;
        g->seconds_left += TG_BONUS_SECONDS;
        place_target(g);
    }

    g->seconds_left -= dt;
    if (g->seconds_left <= 0.0f) {
        g->seconds_left = 0.0f;
        g->over = true;
        g->over_for = 0.0f;
    }
}

void tiltgame_draw(const tiltgame_t *g, canvas_t *c)
{
    canvas_clear(c);

    if (g->over) {
        canvas_puts(c, 1, 0, "TIME UP", PAL_A5);
        char line[32];
        snprintf(line, sizeof line, "caught %d", g->score);
        canvas_puts(c, 1, 2, line, PAL_FG);
        snprintf(line, sizeof line, "best   %d", g->best);
        canvas_puts(c, 1, 3, line, PAL_DIM);
        canvas_puts(c, 1, c->rows - 1, "again in a moment", PAL_DIM);
        return;
    }

    for (int i = 0; i < TG_WALLS; i++)
        canvas_fill_rect(c, g->walls[i].x, g->walls[i].y,
                         g->walls[i].w, g->walls[i].h, PAL_DIM);

    /* The target is a ring rather than a disc, so the ball stays visible as
       it rolls onto it and the two never read as one blob. */
    canvas_circle(c, g->tx, g->ty, TG_TARGET_R, PAL_A4);
    canvas_circle(c, g->tx, g->ty, TG_TARGET_R - 3, PAL_A4);
    canvas_disc(c, (int)g->bx, (int)g->by, TG_BALL_R, PAL_A0);

    /* The clock is a bar that shortens, readable from across a desk in a way
       a number is not, and it turns as it runs out -- two cues, not one. */
    int full = c->w - 2;
    int left = (int)((float)full * g->seconds_left / TG_START_SECONDS);
    if (left > full) left = full;
    if (left < 0) left = 0;
    bool hurry = g->seconds_left < 5.0f;
    canvas_fill_rect(c, 1, 17, left, 4, hurry ? PAL_A5 : PAL_A2);

    char head[32];
    snprintf(head, sizeof head, "%d", g->score);
    canvas_puts(c, 0, 0, head, PAL_FG);
    if (hurry) canvas_puts(c, 4, 0, "HURRY", PAL_A5);
}
