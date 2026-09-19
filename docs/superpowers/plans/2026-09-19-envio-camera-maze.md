# envio Camera + Tilt-Maze Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add two hand-driven, swipe-reachable pages to envio — a QMI8658 tilt-a-ball maze with timer and levels, and a viewfinder-when-held camera that saves JPEGs to the microSD — without disturbing the auto-cycling weather/air dashboard or the other boards.

**Architecture:** Both pages follow the codebase's hardware-free-logic pattern (`pip.c`): a pure module updated with synthetic inputs and unit-tested on the host, with a thin on-device arm in `main.c` that feeds it real sensor/panel data. The maze is fully host-testable and has no hardware unknowns, so it ships first. The camera depends on three hardware unknowns (SD pin numbers, OV5640 SCCB coexistence on the shared I²C bus, and the `esp32-camera` release's SCCB driver generation), so it opens with a bring-up spike that resolves them before the driver and page are written.

**Tech Stack:** ESP-IDF (C11), `esp32-camera` managed component, `driver/i2c_master` (new I²C driver — already used by `i2cbus`), SDMMC 1-bit + FATFS VFS, the project's `canvas` blitter and `pages` model, host unit tests via `host_tests/Makefile` (plain `cc`, `-I../main`).

**Spec:** `docs/superpowers/specs/2026-09-19-envio-camera-maze-design.md`

## Global Constraints

- **Board gate:** all new code compiles on `CONFIG_SCREEN_BOARD_TOUCH_LCD_35B` only. lilly and the CrowPanel must still build green and be byte-for-byte unaffected. ([[boards-stay-independent]])
- **Pins are read from the vendor, never guessed.** DVP pins (verbatim from the spec): XCLK 38, PCLK 41, VSYNC 17, HREF 18, D0–D7 = 45/47/48/46/42/40/39/21, PWDN -1, RESET -1, SCCB SIOD 8 / SIOC 7. SD SDMMC (CLK/CMD/D0) pins are unknown until read off Waveshare's demo download in Task C1 — do not hard-code a guess.
- **SCCB shares GPIO8/7 with `I2CBUS_MAIN`.** The camera must ride the existing bus, not re-init those pins. If the pinned `esp32-camera` release cannot share the new-driver bus, take the fallback (camera owns the bus, other devices attach to its handle) — decided in Task C1.
- **Palette:** envio's blue/amber/white tokens from `palette.h` (`PAL_A0` blue, `PAL_A1` amber, `PAL_FG` white, `PAL_BG` black). No new colours.
- **Panel is native portrait 320×480.** Canvas is 320 wide × 480 tall.
- **Build (envio):** `export PATH="/tmp/py313shim:$PATH" && . /Users/reza/esp/esp-idf/export.sh && idf.py -B build-envio -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.envio" -D SDKCONFIG=sdkconfig.envio build` (the py313 shim exists because system `python3` is 3.14 but the IDF venv is 3.13; recreate with `ln -sf /opt/homebrew/bin/python3.13 /tmp/py313shim/python3` and `.../python`).
- **Flash (envio):** `tools/flash-envio.sh` (app only) — it board-guards by MAC.
- **Host tests:** `cd host_tests && make <target> && ./<target>` — a test prints `ok`/`FAIL` lines and returns non-zero on failure (see `test_pip.c`).
- **Commit cadence:** one commit per task, at the task's end. Attribution footer on every commit:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` then `Claude-Session: https://claude.ai/code/session_011vnnTdcBiNuTKcc8q2WM8d`.

---

## Phase A — Maze (no hardware unknowns; ship first)

### Task A1: Level data and grid model

**Files:**
- Create: `main/maze_levels.h`
- Test: `host_tests/test_maze.c` (created here, grown across A1–A3)
- Modify: `host_tests/Makefile` (add `test_maze` target)

**Interfaces:**
- Produces: `#define MAZE_COLS 10`, `#define MAZE_ROWS 15`, `#define MAZE_CELL 32` (10*32=320, 15*32=480). `typedef struct { uint8_t wall[MAZE_ROWS][MAZE_COLS]; uint8_t start_c, start_r, goal_c, goal_r; } maze_level_t;`. `extern const maze_level_t maze_levels[]; extern const int maze_level_count;` — defined in a new `main/maze_levels.c` (created in this task).

- [ ] **Step 1: Write the failing test**

Add to a new `host_tests/test_maze.c`:

```c
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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd host_tests && make test_maze`
Expected: FAIL — `maze_levels.h` / `maze_levels.c` do not exist (compile error).

- [ ] **Step 3: Write the model and levels**

`main/maze_levels.h`:

```c
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
```

`main/maze_levels.c` — three hand-drawn levels as ASCII art parsed at compile time is awkward in C, so the grids are written as literal rows (`1`=wall, `0`=open). Keep them simple; the border row/col are all `1`:

```c
#include "maze_levels.h"

/* Level 0: a gentle S. Ball starts top-left opening, goal bottom-right. */
const maze_level_t maze_levels[] = {
  { .wall = {
    {1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,1},
    {1,0,1,1,1,1,1,1,0,1},
    {1,0,1,0,0,0,0,0,0,1},
    {1,0,1,0,1,1,1,1,1,1},
    {1,0,0,0,1,0,0,0,0,1},
    {1,1,1,0,1,0,1,1,0,1},
    {1,0,0,0,1,0,1,0,0,1},
    {1,0,1,1,1,0,1,0,1,1},
    {1,0,0,0,0,0,1,0,0,1},
    {1,1,1,1,1,1,1,1,0,1},
    {1,0,0,0,0,0,0,0,0,1},
    {1,0,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1},
  }, .start_c=1, .start_r=1, .goal_c=1, .goal_r=13 },

  /* Level 1: a spiral inward. */
  { .wall = {
    {1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,1},
    {1,0,1,1,1,1,1,1,0,1},
    {1,0,1,0,0,0,0,1,0,1},
    {1,0,1,0,1,1,0,1,0,1},
    {1,0,1,0,1,0,0,1,0,1},
    {1,0,1,0,1,1,1,1,0,1},
    {1,0,1,0,0,0,0,0,0,1},
    {1,0,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,0,1},
    {1,0,0,0,0,0,0,0,0,1},
    {1,0,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1},
  }, .start_c=8, .start_r=1, .goal_c=5, .goal_r=5 },

  /* Level 2: open with pillars — faster, needs control. */
  { .wall = {
    {1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,1},
    {1,0,1,0,1,0,1,0,0,1},
    {1,0,0,0,0,0,0,0,0,1},
    {1,0,1,0,1,0,1,0,1,1},
    {1,0,0,0,0,0,0,0,0,1},
    {1,1,0,1,0,1,0,1,0,1},
    {1,0,0,0,0,0,0,0,0,1},
    {1,0,1,0,1,0,1,0,1,1},
    {1,0,0,0,0,0,0,0,0,1},
    {1,1,0,1,0,1,0,1,0,1},
    {1,0,0,0,0,0,0,0,0,1},
    {1,0,1,0,1,0,1,0,1,1},
    {1,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1},
  }, .start_c=1, .start_r=1, .goal_c=8, .goal_r=13 },
};
const int maze_level_count = (int)(sizeof maze_levels / sizeof maze_levels[0]);
```

Add the `test_maze` target to `host_tests/Makefile` (append to the `all:` list and add a rule):

```make
test_maze: test_maze.c ../main/maze.c ../main/maze_levels.c ../main/canvas.c ../main/textwrap.c
	$(CC) $(CFLAGS) -o $@ $^ -lm
```

(`maze.c` does not exist yet — A2 creates it. Until then, build the target with only the sources that exist by temporarily testing `maze_levels.c` alone; the rule above is the final form used from A2 on. For this task's run, use: `cc $(CFLAGS) -o test_maze test_maze.c ../main/maze_levels.c`.)

- [ ] **Step 4: Run test to verify it passes**

Run: `cd host_tests && cc -std=c11 -Wall -Wextra -Werror -g -I../main -o test_maze test_maze.c ../main/maze_levels.c && ./test_maze`
Expected: PASS — all `ok`, prints `all pass`.

- [ ] **Step 5: Commit**

```bash
git add main/maze_levels.h main/maze_levels.c host_tests/test_maze.c host_tests/Makefile
git commit -m "envio maze: grid model and three hand-drawn levels"
```

---

### Task A2: Ball physics and collision

**Files:**
- Create: `main/maze.h`, `main/maze.c`
- Test: `host_tests/test_maze.c` (extend)

**Interfaces:**
- Consumes: `maze_levels[]`, `maze_level_count`, `MAZE_COLS/ROWS/CELL` from A1.
- Produces:
  - `typedef struct { float bx, by; float vx, vy; int level; float time_s; bool won; } maze_t;` (bx/by = ball centre in canvas pixels; vx/vy = px/s).
  - `void maze_init(maze_t *m);` — level 0, ball at level 0's start cell centre, velocity 0, `time_s`=0, `won`=false.
  - `void maze_restart_level(maze_t *m);` — ball back to the current level's start, velocity 0; leaves `level` and does not clear `won`.
  - `void maze_update(maze_t *m, float gx, float gy, float dt);` — gx/gy are gravity in canvas coords (down is +y), each ~-1..1 at full tilt, as `gravity_from` yields. Integrates velocity, resolves wall collisions axis-by-axis, clamps to the border, accumulates `time_s`. When the ball centre enters the goal cell: if a next level exists, advance `level` and place the ball at its start (velocity 0); on the last level set `won`=true and stop time.
  - `#define MAZE_BALL_R 10` (ball radius, px).

- [ ] **Step 1: Write the failing tests**

Extend `host_tests/test_maze.c` (add these functions and call them from `main`):

```c
#include "maze.h"
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
```

Call all five from `main()` before the summary line.

- [ ] **Step 2: Run to verify it fails**

Run: `cd host_tests && make test_maze`
Expected: FAIL — `maze.h`/`maze.c` missing (compile error).

- [ ] **Step 3: Implement `maze.h` and `maze.c`**

`main/maze.h`:

```c
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
```

`main/maze.c` (draw is stubbed here; A3 fills it and tests it):

```c
#include "maze.h"
#include "palette.h"

#define MAZE_ACCEL 900.0f    /* px/s^2 at full tilt; brisk but controllable */
#define MAZE_FRICTION 3.0f   /* per second; keeps the ball from sliding forever */

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

    /* Move each axis independently and stop that axis on a wall, so the ball
       slides along walls instead of sticking in corners. */
    float nx = m->bx + m->vx * dt;
    if (!blocked(l, nx, m->by)) m->bx = nx; else m->vx = 0.0f;
    float ny = m->by + m->vy * dt;
    if (!blocked(l, m->bx, ny)) m->by = ny; else m->vy = 0.0f;

    m->time_s += dt;

    int bc = (int)(m->bx / MAZE_CELL), br = (int)(m->by / MAZE_CELL);
    if (bc == l->goal_c && br == l->goal_r) {
        if (m->level + 1 < maze_level_count) { m->level++; place_at_start(m); }
        else m->won = true;
    }
}

void maze_draw(canvas_t *c, const maze_t *m) { (void)c; (void)m; }  /* A3 */
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd host_tests && make test_maze && ./test_maze`
Expected: PASS — `all pass`.

- [ ] **Step 5: Commit**

```bash
git add main/maze.h main/maze.c host_tests/test_maze.c
git commit -m "envio maze: ball physics, wall collision, level advance"
```

---

### Task A3: Rendering

**Files:**
- Modify: `main/maze.c` (fill in `maze_draw`)
- Test: `host_tests/test_maze.c` (extend — draw into a real canvas buffer and assert pixels)

**Interfaces:**
- Consumes: `canvas_t` (`canvas_init`, `canvas_fill_rect`, `canvas_disc`, `canvas_puts_px`), `palette.h` tokens.
- Produces: `maze_draw` renders walls (blue), ball (amber), and a white timer string; clears to `PAL_BG` first.

- [ ] **Step 1: Write the failing test**

Add to `host_tests/test_maze.c`:

```c
#include "canvas.h"
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
```

(Add `#include "palette.h"` at the top of the test.)

- [ ] **Step 2: Run to verify it fails**

Run: `cd host_tests && make test_maze && ./test_maze`
Expected: FAIL — `test_draw` fails (draw is a stub, pixels stay 0).

- [ ] **Step 3: Implement `maze_draw`**

Replace the stub in `main/maze.c`:

```c
#include <stdio.h>

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
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd host_tests && make test_maze && ./test_maze`
Expected: PASS — `all pass`.

- [ ] **Step 5: Commit**

```bash
git add main/maze.c host_tests/test_maze.c
git commit -m "envio maze: draw walls, goal, ball and timer"
```

---

### Task A4: Enable the IMU and the maze on envio (build wiring)

**Files:**
- Modify: `main/CMakeLists.txt` (stop excluding `qmi8658.c` on envio; compile `maze.c`/`maze_levels.c` on envio only)
- Modify: `main/pages.h` (add `PAGE_MAZE`)
- Modify: `main/pagedefs.c` (add the `PAGE_MAZE` row, not in the saver cycle)

**Interfaces:**
- Consumes: the `page_def_t` table shape (fields, in order: name, feed, views, `bool wants_stats`, rotate_us, `PG_*` size, `bool menu_tile`, `bool in_saver`).
- Produces: `PAGE_MAZE` enum value, available on envio.

- [ ] **Step 1: Add the enum**

In `main/pages.h`, after `PAGE_LEVEL` (keeping PAGE_COUNT last), add:

```c
    PAGE_MAZE,       /* envio: tilt-a-ball maze on the QMI8658 */
```

- [ ] **Step 2: Add the page_def row**

In `main/pagedefs.c`, add (matching the `PAGE_PIP` row's shape — hand-driven, **`in_saver` = false** so it is swipeable but never auto-cycled):

```c
    [PAGE_MAZE]     = { "Maze",          0,                             NULL,           false, 0,        PG_BOTH,  false, false },
```

- [ ] **Step 3: Fix the CMake excludes**

In `main/CMakeLists.txt`, the `SCREEN_BOARD_TOUCH_LCD_35B` branch currently lists `"qmi8658.c"` in `BOARD_EXCLUDES`; **remove `qmi8658.c` from that branch's list** (leave `level.c particles.c shaketimer.c pip.c gt911.c`).

Then the `CONFIG_SCREEN_ENV_ONLY` block appends `qmi8658.c` again — change it so envio keeps the IMU. Replace:

```cmake
if(CONFIG_SCREEN_ENV_ONLY)
    list(APPEND BOARD_EXCLUDES "qmi8658.c" "level.c" "particles.c" "shaketimer.c")
```

with:

```cmake
if(CONFIG_SCREEN_ENV_ONLY)
    # envio is ENV_ONLY but keeps her soldered QMI8658 for the maze; the other
    # sensor toys stay out.
    if(CONFIG_SCREEN_BOARD_TOUCH_LCD_35B)
        list(APPEND BOARD_EXCLUDES "level.c" "particles.c" "shaketimer.c")
    else()
        list(APPEND BOARD_EXCLUDES "qmi8658.c" "level.c" "particles.c" "shaketimer.c")
    endif()
```

`maze.c`/`maze_levels.c` are new files in `main/` and compile on every board unless excluded. Exclude them everywhere except envio: in the CrowPanel and the `else` board branches add `"maze.c" "maze_levels.c"` to `BOARD_EXCLUDES`, and leave them **out** of the `SCREEN_BOARD_TOUCH_LCD_35B` branch's excludes.

- [ ] **Step 4: Verify all three boards still build**

Run (envio):
`export PATH="/tmp/py313shim:$PATH" && . /Users/reza/esp/esp-idf/export.sh && idf.py -B build-envio -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.envio" -D SDKCONFIG=sdkconfig.envio build`
Expected: builds green. (PAGE_MAZE is defined but not yet dispatched or made available — that is Task A5; a build here proves the enum/def/excludes are consistent.)

Run (lilly, must stay green):
`idf.py -B build-lilly -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.lilly" -D SDKCONFIG=sdkconfig.lilly build`
Expected: builds green, `maze.c` excluded.

- [ ] **Step 5: Commit**

```bash
git add main/pages.h main/pagedefs.c main/CMakeLists.txt
git commit -m "envio: re-enable the QMI8658 and register the Maze page"
```

---

### Task A5: On-device maze page (dispatch + IMU feed + tap-to-reset)

**Files:**
- Modify: `main/main.c` (include `maze.h`; add a `draw_maze` arm and make `PAGE_MAZE` available on envio; tap resets)

**Interfaces:**
- Consumes: `maze_init/update/draw`, `gravity_from(const qmi8658_sample_t*, float*, float*)`, `qmi8658_read`, `touch_tapped`, the page-dispatch and `available`-mask code around `main.c:1935` and the render dispatch.
- Produces: a working Maze page on envio.

- [ ] **Step 1: Make the page available on envio**

In `main/main.c`, in the ENV_ONLY availability block (around line 1935 where `available &= PAGE_BIT(PAGE_CLOCK) | ... | PAGE_BIT(PAGE_AIR);`), add `PAGE_MAZE` to the retained set **only if the IMU is present**. After the existing mask line and the `s_imu` probe, add:

```c
    if (s_imu) available |= PAGE_BIT(PAGE_MAZE);
```

(Place it after the `imu_pages` strip so it is not immediately cleared. Confirm `s_imu` is set by the QMI8658 probe on envio; the probe already runs — see the IMU init near the page-list build.)

- [ ] **Step 2: Add the render arm**

Add a static `maze_t s_maze;` near the other page state, initialise it once (in the same place other page state is initialised, or lazily on first draw with a `static bool inited`). Add a draw function modeled on the Pip arm:

```c
#include "maze.h"

static void draw_maze(canvas_t *c, int64_t now)
{
    static bool inited;
    static int64_t last_us;
    if (!inited) { maze_init(&s_maze); inited = true; last_us = now; }
    float dt = (float)(now - last_us) / 1000000.0f;
    last_us = now;
    if (dt > 0.1f) dt = 0.1f;   /* a long stall must not fling the ball */

    float gx = 0, gy = 0;
    qmi8658_sample_t s;
    if (s_imu && qmi8658_read(&s) == ESP_OK) gravity_from(&s, &gx, &gy);
    maze_update(&s_maze, gx, gy, dt);
    maze_draw(c, &s_maze);
}
```

Wire `PAGE_MAZE` into the page render dispatch (the `switch`/if-chain that calls `draw_clock`, `draw_air`, etc.) to call `draw_maze(c, now)`.

- [ ] **Step 3: Tap resets the current level**

In the tap-handling block (where `touch_tapped()` is consumed), when the current page is `PAGE_MAZE`, call `maze_restart_level(&s_maze)` instead of the default tap action, and do not treat it as a page change. (Swipes still page away — `touch_swipe` is handled separately.)

- [ ] **Step 4: Build, flash, and verify on hardware**

Run: `export PATH="/tmp/py313shim:$PATH" && . /Users/reza/esp/esp-idf/export.sh && idf.py -B build-envio -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.envio" -D SDKCONFIG=sdkconfig.envio build && tools/flash-envio.sh`
Expected: builds green, flashes. On the board: swipe past Air to reach Maze; tilting rolls the amber ball through the blue walls; reaching the amber goal advances the level; the timer counts; a tap resets the level. The dashboard's 20 s auto-cycle never lands on Maze.

- [ ] **Step 5: Commit**

```bash
git add main/main.c
git commit -m "envio maze: on-device page, IMU gravity feed, tap to reset"
```

---

## Phase B — Camera (spike-gated)

### Task B1: Camera + SD bring-up spike

**Goal of the spike (output is knowledge + one throwaway-quality end-to-end path, not polished code):** resolve the three unknowns and record them, so B2–B4 can be written against real APIs.

**Files:**
- Modify: `main/idf_component.yml` (add `espressif/esp32-camera`)
- Modify: `sdkconfig.defaults.envio` (camera + FATFS/SDMMC config as needed)
- Create (throwaway, folded into B2/B3 after): a temporary bring-up path callable from boot on envio.
- Modify: [[envio-board]] memory (record the confirmed SD pins, SCCB decision, and `esp32-camera` version).

- [ ] **Step 1: Read the SD pin numbers from Waveshare's source**

Download Waveshare's ESP32-S3-Touch-LCD-3.5B demo (from the wiki's demo link) or open the board schematic, and find `SD_MMC.setPins(clk, cmd, d0)` (Arduino demo) or the ESP-IDF BSP's SDMMC slot config. Record the three GPIO numbers. Cross-check they do not collide with the DVP pins (38,41,17,18,45,47,48,46,42,40,39,21), the QSPI panel (1,2,3,4,5,12), the backlight (6), or the I²C bus (7,8).
Expected output: three confirmed GPIO numbers written into this task's notes and into [[envio-board]].

- [ ] **Step 2: Add the component and discover its SCCB interface**

Add to `main/idf_component.yml`:

```yaml
  espressif/esp32-camera: "^2.0.0"
```

Run the envio build once so the component resolves and locks. Then inspect the resolved component's `camera_config_t` in `managed_components/espressif__esp32-camera/driver/include/esp_camera.h`: confirm whether it exposes `sccb_i2c_port` (an existing-port field) and which I²C driver its SCCB layer uses (`CONFIG_SCCB_HARDWARE_I2C_PORT*` / legacy `driver/i2c` vs new `driver/i2c_master`).
Expected output: a note stating (a) the exact field name for reusing an existing bus/port, and (b) whether it is compatible with `i2cbus`'s new-driver `I2CBUS_MAIN`.

- [ ] **Step 3: Decide SCCB coexistence and prove one frame**

Write a temporary `camera_spike()` called once at boot on envio (guarded by `#if CONFIG_SCREEN_BOARD_TOUCH_LCD_35B`). Fill `camera_config_t` with the DVP pins from Global Constraints and:
- **Primary:** set `sccb_i2c_port` to the port `i2cbus` opened for `I2CBUS_MAIN`, set `pin_sccb_sda`/`pin_sccb_scl` to -1, `pixel_format = PIXFORMAT_RGB565`, `frame_size = FRAMESIZE_QVGA`, `fb_location = CAMERA_FB_IN_PSRAM`, `fb_count = 1`, `xclk_freq_hz = 20000000`.
- Call `esp_camera_init`, then `esp_camera_fb_get`; log the returned `width`/`height`/`len`; `esp_camera_fb_return`; `esp_camera_deinit`.
- If init fails on SCCB, switch to the **fallback**: let `esp32-camera` own pins 8/7 (`pin_sccb_sda=8`, `pin_sccb_scl=7`), and confirm a frame; record that the other I²C devices must then attach to the camera's bus handle (a larger change scoped in B2 if taken).

Flash and read the log.
Expected output: a log line with a non-zero frame (e.g., `320x240 len=153600`), and a recorded decision: **shared port** or **fallback**.

- [ ] **Step 4: Prove one JPEG to SD**

Extend `camera_spike()`: mount SDMMC 1-bit at `/sdcard` with the pins from Step 1 (`esp_vfs_fat_sdmmc_mount` with a 1-line slot config, width 1), reinit the sensor to `PIXFORMAT_JPEG` at a mid frame size, grab one fb, write it to `/sdcard/spike.jpg`, return the fb, unmount. Flash, run, then pull the card and confirm `spike.jpg` opens as an image.
Expected output: a valid JPEG on the card. Record any SDMMC quirks (pull-ups, `sdmmc_host_t` flags) in [[envio-board]].

- [ ] **Step 5: Record findings and commit the scaffolding**

Delete or comment out `camera_spike()` (it is thrown away; B2/B3 write the real modules). Keep `idf_component.yml`, `dependencies.lock`, and the `sdkconfig.defaults.envio` changes. Update [[envio-board]] with: confirmed SD pins, the SCCB decision, the `esp32-camera` version, and any SDMMC quirks.

```bash
git add main/idf_component.yml main/dependencies.lock sdkconfig.defaults.envio
git commit -m "envio camera: bring-up spike — SD pins, SCCB coexistence, one frame + one JPEG confirmed"
```

> **Gate:** B2–B4 are written against the SCCB decision from this task. If the fallback was taken, B2 additionally re-routes the existing I²C sensors onto the camera's bus handle; note that here before proceeding.

---

### Task B2: SD card module

**Files:**
- Create: `main/sdcard.h`, `main/sdcard.c`
- Modify: `main/CMakeLists.txt` (compile on envio only)

**Interfaces:**
- Produces:
  - `esp_err_t sd_mount(void);` — mounts 1-bit SDMMC at `/sdcard` once (idempotent; returns `ESP_OK` if already mounted). Uses the confirmed pins from B1.
  - `esp_err_t sd_write(const char *path, const uint8_t *data, size_t len);` — writes a file under `/sdcard`, creating parent dirs as needed.
  - `void sd_photo_name(char *out, size_t n);` — formats `envio/IMG_YYYYMMDD_HHMMSS.jpg` from the DS3231 clock; if the clock is unset, uses `envio/IMG_boot_<seq>.jpg` with a static incrementing `<seq>`.

- [ ] **Step 1: Implement `sdcard.h`**

```c
#ifndef SDCARD_H
#define SDCARD_H
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
esp_err_t sd_mount(void);
esp_err_t sd_write(const char *path, const uint8_t *data, size_t len);
void sd_photo_name(char *out, size_t n);   /* "envio/IMG_YYYYMMDD_HHMMSS.jpg" */
#endif
```

- [ ] **Step 2: Implement `sdcard.c`**

Implement `sd_mount` with `esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &opts, &card)` (SDMMC host, 1-bit slot with the B1 pins, `format_if_mount_failed=false`, `max_files=4`), guarding re-mount with a `static bool mounted`. Implement `sd_write` with `mkdir` of the parent under `/sdcard` (ignoring `EEXIST`) then `fopen`/`fwrite`/`fclose`, mapping errors to `esp_err_t`. Implement `sd_photo_name` by reading the DS3231 (reuse the existing `ds3231` read path used at `main.c:850`) into a `ds3231_date_t` plus time, and `snprintf`-ing the name; fall back to the boot sequence when the year is < 2000.

- [ ] **Step 3: Compile on envio only**

In `main/CMakeLists.txt`, add `"sdcard.c"` to `BOARD_EXCLUDES` in the CrowPanel and `else` board branches, and leave it out of the `SCREEN_BOARD_TOUCH_LCD_35B` branch.

- [ ] **Step 4: Build and smoke-test on hardware**

Temporarily call `sd_mount()` then `sd_write("envio/hello.txt", (const uint8_t*)"hi", 2)` from boot on envio; build, flash, and confirm `envio/hello.txt` appears on the card with `hi`. Remove the temporary call.
Run: the envio build + `tools/flash-envio.sh`.
Expected: file present on the card.

- [ ] **Step 5: Commit**

```bash
git add main/sdcard.h main/sdcard.c main/CMakeLists.txt
git commit -m "envio: microSD module — mount, write, timestamped photo names"
```

---

### Task B3: Camera module

**Files:**
- Create: `main/camera.h`, `main/camera.c`
- Modify: `main/CMakeLists.txt` (compile on envio only)

**Interfaces:**
- Consumes: `esp32-camera` (`esp_camera_init/deinit/fb_get/fb_return`, `esp_camera_sensor_get`, `sensor_t::set_pixformat/set_framesize`), the SCCB decision from B1, `i2cbus` for the shared port (primary path).
- Produces:
  - `esp_err_t camera_start(void);` — `esp_camera_init` in RGB565 QVGA on the shared SCCB port; idempotent.
  - `void camera_stop(void);` — `esp_camera_deinit`; safe if not started.
  - `bool camera_preview(canvas_t *c);` — grabs one RGB565 frame and blits it centred into the canvas; returns false if no frame. (Blit uses `canvas_blit` from Task B0-in-A? No — add `canvas_blit` here; see Step 1.)
  - `esp_err_t camera_capture_jpeg(const uint8_t **out, size_t *len, camera_fb_t **fb_to_return);` — switches the sensor to JPEG at capture res, grabs a frame, returns its buffer for the caller to write, and hands back the `fb` so the caller returns it; the caller then calls `camera_resume_preview()`.
  - `void camera_resume_preview(void);` — switches the sensor back to RGB565 QVGA.

- [ ] **Step 1: Add a canvas blit helper (host-tested)**

Add to `main/canvas.h`:

```c
/* Copies an sw x sh RGB565 image to (dx,dy), clipped to the panel. For the
   camera viewfinder. */
void canvas_blit(canvas_t *c, const uint16_t *src, int sw, int sh, int dx, int dy);
```

Implement in `main/canvas.c` (row-by-row copy with clipping to `c->w`/`c->h`). Add a host test `test_canvas.c` case: blit a 2×2 image into a cleared 320×480 canvas at (10,10) and assert those four pixels match and a pixel outside is untouched. Run `make test_canvas && ./test_canvas`; expect PASS.

- [ ] **Step 2: Implement `camera.h`** (as the Interfaces block above).

- [ ] **Step 3: Implement `camera.c`**

`camera_start` fills `camera_config_t` with the DVP pins (Global Constraints) and the SCCB per the B1 decision; RGB565/QVGA/PSRAM/`fb_count=1`; idempotent via a `static bool started`. `camera_preview` does `esp_camera_fb_get` → `canvas_blit(c, (uint16_t*)fb->buf, fb->width, fb->height, (320-fb->width)/2, (480-fb->height)/2)` → `esp_camera_fb_return`. Capture switches pixformat/framesize via `esp_camera_sensor_get()`, grabs, returns buffer+fb to the caller. `camera_resume_preview` switches back.

- [ ] **Step 4: Compile on envio only**

Add `"camera.c"` to `BOARD_EXCLUDES` on the non-envio branches; leave out of the envio branch. Add a `CONFIG_SCREEN_HAVE_CAMERA` bool in `Kconfig.projbuild`, `default y if SCREEN_BOARD_TOUCH_LCD_35B`, and guard `camera.c`'s body and its includes in `main.c` with it.

- [ ] **Step 5: Build to verify it links**

Run the envio build.
Expected: builds green (module compiled, not yet dispatched). lilly build still green with `camera.c` excluded.

- [ ] **Step 6: Commit**

```bash
git add main/camera.h main/camera.c main/canvas.h main/canvas.c host_tests/test_canvas.c main/CMakeLists.txt main/Kconfig.projbuild
git commit -m "envio: camera module (RGB565 preview, JPEG capture) and canvas blit"
```

---

### Task B4: Camera page (viewfinder-when-held + tap-to-capture)

**Files:**
- Modify: `main/pages.h` (add `PAGE_CAMERA`)
- Modify: `main/pagedefs.c` (row, `in_saver`=false)
- Modify: `main/main.c` (availability, lifecycle on page enter/leave, viewfinder draw, tap = shutter)

**Interfaces:**
- Consumes: `camera_start/stop/preview/capture_jpeg/resume_preview`, `sd_mount/sd_write/sd_photo_name`, the page enter/leave hook (detect current-page change to start/stop the camera), `touch_tapped`.
- Produces: a working Camera page on envio.

- [ ] **Step 1: Register the page**

`main/pages.h`: add `PAGE_CAMERA,` (before `PAGE_COUNT`). `main/pagedefs.c`: add
`[PAGE_CAMERA] = { "Camera", 0, NULL, false, 0, PG_BOTH, false, false },` (in_saver=false → swipeable, not auto-cycled). In `main.c` availability, add `available |= PAGE_BIT(PAGE_CAMERA);` on envio guarded by `#if CONFIG_SCREEN_HAVE_CAMERA`.

- [ ] **Step 2: Lifecycle — start on enter, stop on leave**

envio's loop tracks the current page. Add a `static page_t s_prev_page;` compare each loop: when `current == PAGE_CAMERA` and it just became so, call `camera_start()` and `sd_mount()`; when it just stopped being `PAGE_CAMERA`, call `camera_stop()`. This keeps the camera off during the auto-cycle and powered down when unheld.

- [ ] **Step 3: Viewfinder + capture**

Add `draw_camera(canvas_t *c)`: `canvas_clear(c)`, then `if (!camera_preview(c)) canvas_puts_px(c, 6, 2, "camera warming up", PAL_FG);` and a caption line `canvas_puts_px(c, 6, 460, "tap to capture", PAL_A1);`. Wire `PAGE_CAMERA` into the render dispatch. In the tap handler, when the current page is `PAGE_CAMERA`, run the shutter: `camera_capture_jpeg` → `sd_photo_name` → `sd_write` → return fb → `camera_resume_preview`, then show a brief `canvas_puts_px(... "saved <name>" ...)` for a frame or two (a `static int64_t s_shot_msg_until`), and do not treat the tap as a page change.

- [ ] **Step 4: Build, flash, verify on hardware**

Run the envio build + `tools/flash-envio.sh`.
Expected: swipe past Maze to reach Camera; a live preview appears while the page is held; a tap saves a timestamped JPEG to `/sdcard/envio/` and shows a "saved" confirmation; swiping away stops the camera (verify via log that `esp_camera_deinit` ran and the dashboard resumes at full speed); the auto-cycle never lands on Camera. Pull the card and confirm the JPEGs open.

- [ ] **Step 5: Commit**

```bash
git add main/pages.h main/pagedefs.c main/main.c
git commit -m "envio camera: viewfinder-when-held page, tap to save a JPEG to SD"
```

---

## Self-Review

**Spec coverage:**
- Camera viewfinder-when-held → B3 (`camera_preview`) + B4 (lifecycle on page enter/leave). ✓
- Tap to capture JPEG to SD → B3 (`camera_capture_jpeg`) + B2 (`sd_write`/`sd_photo_name`) + B4 (shutter). ✓
- SCCB shares GPIO8/7 / coexistence primary+fallback → B1 (spike, decision recorded). ✓
- SD 1-bit SDMMC, timestamped names, `/sdcard/envio/` → B2. ✓
- DVP pinout verbatim → Global Constraints, used in B1/B3. ✓
- SD pins from vendor, not guessed → B1 Step 1. ✓
- Lazy camera lifecycle / powered down when unused → B4 Step 2. ✓
- Maze: IMU tilt-ball, timer, levels, hand-drawn → A1–A5. ✓
- Re-enable soldered QMI8658 on envio → A4 Step 3. ✓
- Tap resets level, swipe pages away → A5 Step 3. ✓
- Swipeable, not auto-cycled (both pages) → A4/B4 via `in_saver=false`. ✓
- Blue/amber/white palette → A3, B4. ✓
- Board-gated, other boards unaffected → A4/B2/B3 CMake excludes + Kconfig gate, verified by lilly builds in A4. ✓
- Order: maze first, then camera spike → Phase A before Phase B. ✓

**Placeholder scan:** No "TBD"/"handle errors"/"similar to". The one genuinely unknown value (SD pins) is a first-step vendor lookup, not a code placeholder; every code step carries real code.

**Type consistency:** `maze_t` fields (`bx,by,vx,vy,level,time_s,won`) and functions (`maze_init/restart_level/update/draw`) are consistent A2↔A3↔A5. `canvas_blit` signature is identical in B3 Step 1 (canvas.h) and its use in `camera.c`. `sd_mount/sd_write/sd_photo_name` and `camera_start/stop/preview/capture_jpeg/resume_preview` names match between B2/B3 definitions and B4 uses.
