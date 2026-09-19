#ifndef MAZE_LEVELS_H
#define MAZE_LEVELS_H
#include <stdint.h>

/* A 10x15 grid of 32px cells fills the 320x480 portrait exactly. 1 is wall,
   0 is open. Hand-drawn: the border is always walled so the ball cannot leave. */
#define MAZE_COLS 10
#define MAZE_ROWS 15
#define MAZE_CELL 32

typedef struct {
    uint8_t wall[MAZE_ROWS][MAZE_COLS];
    uint8_t start_c, start_r;   /* opening cell the ball begins in */
    uint8_t goal_c, goal_r;     /* the cell to reach */
} maze_level_t;

extern const maze_level_t maze_levels[];
extern const int maze_level_count;
#endif
