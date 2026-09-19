#include "maze.h"
#include "palette.h"
#include <math.h>
#include <stdio.h>

#define MAZE_ACCEL 3600.0f   /* px/s^2 at full tilt; snappy, ~1.8k px/s top speed */
#define MAZE_FRICTION 2.0f   /* per second; keeps the ball from sliding forever */

static void place_at_start(maze_t *m) {
    const maze_level_t *l = &maze_levels[m->level];
    m->bx = (l->start_c + 0.5f) * MAZE_CELL;
    m->by = (l->start_r + 0.5f) * MAZE_CELL;
    m->vx = m->vy = 0.0f;
}

void maze_init(maze_t *m) {
    m->level = 0; m->time_s = 0.0f; m->won = false;
    place_at_start(m);
}
void maze_restart_level(maze_t *m) { place_at_start(m); }

static bool wall_at_px(const maze_level_t *l, float x, float y) {
    int c = (int)(x / MAZE_CELL), r = (int)(y / MAZE_CELL);
    if (c < 0 || c >= MAZE_COLS || r < 0 || r >= MAZE_ROWS) return true;
    return l->wall[r][c] != 0;
}

/* True if a ball centred at (x,y) with radius R overlaps any wall cell. Checks
   the four points at the ball's edges — enough for axis-separated resolution. */
static bool blocked(const maze_level_t *l, float x, float y) {
    return wall_at_px(l, x - MAZE_BALL_R, y) || wall_at_px(l, x + MAZE_BALL_R, y)
        || wall_at_px(l, x, y - MAZE_BALL_R) || wall_at_px(l, x, y + MAZE_BALL_R);
}

void maze_update(maze_t *m, float gx, float gy, float dt) {
    if (m->won) return;
    const maze_level_t *l = &maze_levels[m->level];

    m->vx += MAZE_ACCEL * gx * dt;
    m->vy += MAZE_ACCEL * gy * dt;
    float damp = 1.0f - MAZE_FRICTION * dt;
    if (damp < 0.0f) damp = 0.0f;
    m->vx *= damp; m->vy *= damp;

    /* Move in sub-steps no larger than the ball's radius, so a fast ball cannot
       tunnel through a wall in a single frame. Each sub-step resolves each axis
       independently, so the ball still slides along walls rather than sticking. */
    float dx = m->vx * dt, dy = m->vy * dt;
    float reach = fabsf(dx) > fabsf(dy) ? fabsf(dx) : fabsf(dy);
    int steps = (int)(reach / MAZE_BALL_R) + 1;
    float sx = dx / steps, sy = dy / steps;
    for (int s = 0; s < steps; s++) {
        float nx = m->bx + sx;
        if (!blocked(l, nx, m->by)) m->bx = nx; else { m->vx = 0.0f; sx = 0.0f; }
        float ny = m->by + sy;
        if (!blocked(l, m->bx, ny)) m->by = ny; else { m->vy = 0.0f; sy = 0.0f; }
    }

    m->time_s += dt;

    int bc = (int)(m->bx / MAZE_CELL), br = (int)(m->by / MAZE_CELL);
    if (bc == l->goal_c && br == l->goal_r) {
        if (m->level + 1 < maze_level_count) { m->level++; place_at_start(m); }
        else m->won = true;
    }
}

void maze_draw(canvas_t *c, const maze_t *m) {
    canvas_clear(c);
    const maze_level_t *l = &maze_levels[m->level];
    for (int r = 0; r < MAZE_ROWS; r++)
        for (int col = 0; col < MAZE_COLS; col++)
            if (l->wall[r][col])
                canvas_fill_rect(c, col*MAZE_CELL, r*MAZE_CELL, MAZE_CELL, MAZE_CELL, PAL_A0);
    /* goal marker: an amber ring cell so it reads before you reach it */
    canvas_fill_rect(c, l->goal_c*MAZE_CELL+8, l->goal_r*MAZE_CELL+8,
                     MAZE_CELL-16, MAZE_CELL-16, PAL_A1);
    canvas_disc(c, (int)m->bx, (int)m->by, MAZE_BALL_R, PAL_A1);

    char line[32];
    if (m->won) snprintf(line, sizeof line, "WON %.1fs", (double)m->time_s);
    else        snprintf(line, sizeof line, "L%d  %.1fs", m->level + 1, (double)m->time_s);
    canvas_puts_px(c, 6, 2, line, PAL_FG);
}
