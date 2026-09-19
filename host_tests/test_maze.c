#include "maze_levels.h"
#include <stdio.h>
static int failures;
static void expect(const char *what, int cond) {
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what); failures++;
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
int main(void) { test_levels(); printf("%s\n", failures ? "FAILURES" : "all pass"); return failures ? 1 : 0; }
