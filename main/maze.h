#ifndef MAZE_H
#define MAZE_H
#include "canvas.h"
#include "maze_levels.h"
#include <stdbool.h>

#define MAZE_BALL_R 10

/* A tilt-a-ball maze, free of hardware like pip: fed gravity in canvas coords
   and a timestep, it moves the ball, collides it with the walls, and advances
   levels. The board reads the QMI8658 and turns it into gravity; the maze
   never sees a sensor. */
typedef struct {
    float bx, by;     /* ball centre, canvas pixels */
    float vx, vy;     /* velocity, px/s */
    int   level;
    float time_s;     /* run time; stops when won */
    bool  won;
} maze_t;

void maze_init(maze_t *m);
void maze_restart_level(maze_t *m);
void maze_update(maze_t *m, float gx, float gy, float dt);
void maze_draw(canvas_t *c, const maze_t *m);
#endif
