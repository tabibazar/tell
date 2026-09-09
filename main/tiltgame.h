#ifndef TILTGAME_H
#define TILTGAME_H

#include "canvas.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Tilt the board to roll a ball around a walled arena and catch rings before
 * the clock runs out. Every catch adds a few seconds, so a good run keeps
 * itself alive and a bad one ends. There is no button on this board, so a
 * finished round starts a new one by itself.
 *
 * Pure C, handed a gravity vector, so it plays on the host in a test exactly
 * as it does on the board.
 */

#define TG_WALLS 4
#define TG_START_SECONDS 25.0f
#define TG_BONUS_SECONDS 4.0f
#define TG_BALL_R 5
#define TG_TARGET_R 7
/* A finished round holds its score this long before restarting. */
#define TG_RESTART_AFTER 6.0f

typedef struct { int x, y, w, h; } tg_rect_t;

typedef struct {
    float bx, by;            /* the ball */
    float vx, vy;
    int tx, ty;              /* the ring to catch */
    tg_rect_t walls[TG_WALLS];
    int score;
    int best;
    float seconds_left;
    bool over;
    float over_for;
    int w, h;
    uint32_t rng;
} tiltgame_t;

void tiltgame_init(tiltgame_t *g, int w, int h, uint32_t seed);

/* A fresh round. The best score survives it. */
void tiltgame_restart(tiltgame_t *g);

/* Gravity in pixels per second squared, panel coordinates. */
void tiltgame_step(tiltgame_t *g, float gx, float gy, float dt);

void tiltgame_draw(const tiltgame_t *g, canvas_t *c);

#endif /* TILTGAME_H */
