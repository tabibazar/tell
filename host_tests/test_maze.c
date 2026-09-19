#include "maze.h"
#include "maze_levels.h"
#include "canvas.h"
#include "palette.h"
#include <stdio.h>
static int failures;
static void expect(const char *what, int cond) {
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what); failures++;
}
static void step(maze_t *m, float gx, float gy, float secs) {
    for (float t = 0; t < secs; t += 0.02f) maze_update(m, gx, gy, 0.02f);
}
static void test_gravity(void) {
    maze_t m; maze_init(&m);
    float x0 = m.bx;
    step(&m, 1.0f, 0.0f, 0.3f);              /* tilt right */
    expect("tilt right moves ball right", m.bx > x0);
}
static void test_wall_stops(void) {
    maze_t m; maze_init(&m);                  /* level 0 start is cell (1,1) */
    step(&m, 1.0f, 0.0f, 3.0f);               /* shove right for a long time */
    /* cell (2,1) is open, (2,2) wall row below; ball cannot pass the right border */
    expect("ball never leaves right border", m.bx <= (MAZE_COLS-1)*MAZE_CELL);
    expect("ball never leaves left border",  m.bx >= MAZE_CELL);
}
static void test_timer(void) {
    maze_t m; maze_init(&m);
    step(&m, 0.0f, 0.0f, 1.0f);
    expect("timer runs", m.time_s > 0.9f && m.time_s < 1.2f);
}
static void test_goal_advances(void) {
    maze_t m; maze_init(&m);
    /* Teleport onto the goal cell centre and step once. */
    m.bx = (maze_levels[0].goal_c + 0.5f) * MAZE_CELL;
    m.by = (maze_levels[0].goal_r + 0.5f) * MAZE_CELL;
    int before = m.level;
    maze_update(&m, 0, 0, 0.02f);
    expect("reaching goal advances the level", m.level == before + 1);
    expect("not won until last level", !m.won);
}
static void test_win_on_last(void) {
    maze_t m; maze_init(&m);
    m.level = maze_level_count - 1;
    maze_restart_level(&m);
    m.bx = (maze_levels[m.level].goal_c + 0.5f) * MAZE_CELL;
    m.by = (maze_levels[m.level].goal_r + 0.5f) * MAZE_CELL;
    maze_update(&m, 0, 0, 0.02f);
    expect("last goal wins", m.won);
}
static void test_levels(void) {
    expect("at least 3 levels", maze_level_count >= 3);
    for (int i = 0; i < maze_level_count; i++) {
        const maze_level_t *l = &maze_levels[i];
        expect("start in bounds", l->start_c < MAZE_COLS && l->start_r < MAZE_ROWS);
        expect("goal in bounds",  l->goal_c  < MAZE_COLS && l->goal_r  < MAZE_ROWS);
        expect("start cell is open", l->wall[l->start_r][l->start_c] == 0);
        expect("goal cell is open",  l->wall[l->goal_r][l->goal_c]  == 0);
        expect("border is walled",   l->wall[0][0] == 1 && l->wall[MAZE_ROWS-1][MAZE_COLS-1] == 1);
    }
}
static void test_draw(void) {
    static uint16_t fb[320*480];
    canvas_t c; canvas_init(&c, fb, 320, 480, 1);
    maze_t m; maze_init(&m);
    maze_draw(&c, &m);
    /* border cell (0,0) is wall -> blue somewhere in its 32x32 block */
    int found_wall = 0;
    for (int y = 0; y < MAZE_CELL; y++)
        for (int x = 0; x < MAZE_CELL; x++)
            if (fb[y*320+x] == PAL_A0) found_wall = 1;
    expect("walls drawn in blue", found_wall);
    /* ball centre pixel is amber */
    expect("ball drawn in amber", fb[(int)m.by*320 + (int)m.bx] == PAL_A1);
}
int main(void) {
    test_gravity();
    test_wall_stops();
    test_timer();
    test_goal_advances();
    test_win_on_last();
    test_levels();
    test_draw();
    printf("%s\n", failures ? "FAILURES" : "all pass");
    return failures ? 1 : 0;
}
