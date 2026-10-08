# panel1: a page per room, and sleep — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** panel1 gets a full-width page for each meeting room, opened from the
overview's room headers. The screen also goes dark after 5 minutes untouched
and wakes on BOOT, PWR or a tap.

**Spec:** `docs/superpowers/specs/2026-10-08-panel1-room-pages-design.md`.

**Architecture:** all drawing and hit-testing stays in the pure, host-tested
`panel1/main/roomsui.c`. The room page reuses the overview's grid, booking
blocks and "taken till" logic, which are first factored out of
`roomsui_draw`. `panel1/main/panel1.c` keeps the state (`s_room`,
`s_asleep`) and the touch loop. `panel1/main/lcd.c` gains a read of the PWR
key through the TCA9554 it already drives.

**Tech stack:** ESP-IDF 5.5 (C), host tests in `host_tests/` built with `cc`.

**Panel fonts are ASCII only.** The spec's "·" and "–" become ", ", " - " and
"-" on screen.

## Commands used throughout

- Host test: `cd /Users/reza/stan/tell/host_tests && make test_rooms && cd .. && ./host_tests/test_rooms`.
  It must be run from the repo root, because it writes renders to
  `host_tests/renders/rooms/`.
- Firmware build (the Python env must be named; see memory
  `idf-export-python-env`):
  ```bash
  cd /Users/reza/stan/tell/panel1 && export IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf5.5_py3.13_env PATH=$HOME/.espressif/python_env/idf5.5_py3.13_env/bin:$PATH && . ~/esp/esp-idf/export.sh >/dev/null 2>&1; idf.py build 2>&1 | tail -3
  ```
- Flash: `git fetch` first and make sure nothing new has landed on
  `origin/main`, then `tools/flash-panel1.sh`. panel1 must be on its CH343
  port (`/dev/cu.usbmodem5B91*`).

## Files

- Modify `panel1/main/roomsui.h`: the new functions `roomsui_room_draw`,
  `roomsui_room_status`, `roomsui_head_hit`, `roomsui_room_hit` and
  `roomsui_room_head_hit`. `roomsui_saver` goes.
- Modify `panel1/main/roomsui.c`: the shared helpers, the room page, and the
  saver removed.
- Modify `panel1/main/fonts.c`; delete `panel1/main/font_rooms_clock.h`.
- Modify `panel1/main/lcd.h` / `panel1/main/lcd.c`: `lcd_pwr_key`.
- Modify `panel1/main/panel1.c`: room navigation, sleep and the buttons.
- Modify `host_tests/test_rooms.c`.

---

### Task 1: Factor the overview's grid, blocks and hits into helpers (no change on screen)

**Files:** modify `panel1/main/roomsui.c`.

- [ ] **Step 1: Save today's renders as the baseline**

```bash
cd /Users/reza/stan/tell/host_tests && make test_rooms >/dev/null && cd .. && ./host_tests/test_rooms
mkdir -p /tmp/rooms-before && cp host_tests/renders/rooms/*.bmp /tmp/rooms-before/
```
Expected: `rooms: all passed`.

- [ ] **Step 2: Add the helpers above `roomsui_draw`**

Put these after `col_span` in `roomsui.c`:

```c
/* Minute m's y on a grid of hours first..last drawn from top to bottom. */
static int y_of(int m, int first, int last, int top, int bottom)
{
    return top + (m - first * 60) * (bottom - top) / ((last - first) * 60);
}

/* Hour rules and figures, half hours fainter. */
static void draw_grid(canvas_t *c, int first, int last, int top, int bottom)
{
    const aafont_t *lf = &aafont_rooms_small;
    for (int hr = first; hr <= last; hr++) {
        int y = y_of(hr * 60, first, last, top, bottom);
        canvas_fill_rect(c, GUTTER - 2, y, W - GUTTER - MARGIN + 2, 1, C_RULE);
        if (hr < last) canvas_fill_rect(c, GUTTER, y_of(hr * 60 + 30, first, last, top, bottom), W - GUTTER - MARGIN, 1, C_HALF);
        char t[12];
        snprintf(t, sizeof t, "%d", hr);
        int ty = y - lf->cap / 2;
        if (ty < top) ty = top;
        if (hr < last) aafont_draw(c, lf, GUTTER - 6, ty, t, C_DIM, AAFONT_RIGHT);
    }
}

/* The red line at the minute, today only. */
static void draw_now(canvas_t *c, const roomsui_view_t *v, int first, int last, int top, int bottom)
{
    if (v->offset != 0 || v->now < first * 60 || v->now > last * 60) return;
    int y = y_of(v->now, first, last, top, bottom);
    canvas_fill_rect(c, GUTTER - 4, y - 1, W - GUTTER - MARGIN + 4, 2, C_NOW);
    canvas_disc(c, GUTTER - 4, y, 4, C_NOW);
}

/* "No bookings" in the middle of an empty grid. */
static void draw_empty(canvas_t *c, const roomsui_view_t *v, int first, int last, int top, int bottom)
{
    const char *msg = v->wday == 0 || v->wday == 6 ? "No bookings - it's the weekend" : "No bookings";
    int y = y_of(first * 60 + (last - first) * 30, first, last, top, bottom) - aafont_inter_sub.cap / 2;
    int tw = aafont_width(&aafont_inter_sub, msg);
    canvas_fill_rect(c, (W + GUTTER - tw) / 2 - 12, y - 10, tw + 24, aafont_inter_sub.cap + 20, C_BG);
    aafont_draw(c, &aafont_inter_sub, (W + GUTTER) / 2, y, msg, C_DIM, AAFONT_CENTRE);
}

/* 0 to come, 1 on now, 2 over. */
static int ev_state(const room_ev_t *e, const roomsui_view_t *v)
{
    bool today = v->offset == 0 && v->now >= 0;
    if (!today) return v->offset < 0 ? 2 : 0;
    return e->end <= v->now ? 2 : e->start <= v->now ? 1 : 0;
}

/* A booking's block and its bar; its text colours in *fg and *who. */
static void draw_block(canvas_t *c, int x, int y, int w, int h, int state, uint16_t *fg, uint16_t *who)
{
    uint16_t bg = state == 1 ? C_ON : state == 2 ? C_PAST : C_EV;
    uint16_t bar = state == 1 ? C_ON_BAR : state == 2 ? C_PAST_BAR : C_EV_BAR;
    *fg = state == 2 ? C_DIM : C_TEXT;
    *who = state == 2 ? C_FAINT : C_EV_WHO;
    round_rect(c, x, y, w, h, 4, bg);
    canvas_fill_rect(c, x, y + 1, 3, h - 2, bar);
}

/* The booking in r nearest y whose block, grown to 28 px for a finger, holds y. */
static const room_ev_t *hit_in(const room_t *r, int y, int first, int last, int top, int bottom)
{
    const room_ev_t *best = NULL;
    int bestd = 1 << 30;
    for (int i = 0; i < r->n; i++) {
        const room_ev_t *e = &r->ev[i];
        int y0 = y_of(e->start, first, last, top, bottom), y1 = y_of(e->end, first, last, top, bottom);
        int mid = (y0 + y1) / 2, half = (y1 - y0) / 2 < 14 ? 14 : (y1 - y0) / 2;
        int d = abs(y - mid);
        if (d <= half && d < bestd) { best = e; bestd = d; }
    }
    return best;
}

/* Till when r is taken from `now`, running on through back-to-back
   bookings; -1 when it is free now. */
static int taken_till(const room_t *r, int now)
{
    const room_ev_t *on = rooms_at(r, now);
    if (!on) return -1;
    int till = on->end;
    for (int i = 0; i < r->n; i++)
        if (r->ev[i].start <= till && r->ev[i].end > till) till = r->ev[i].end;
    return till;
}
```

- [ ] **Step 3: Make the existing code use them**

In `draw_room_head`, replace the `if (on) { ... }` branch body with:

```c
        if (on) {
            int till = taken_till(r, v->now);
            snprintf(line, sizeof line, "till %d:%02d", till / 60, till % 60);
```

In `draw_booking`, replace its first six lines (the colours, `round_rect`
and the bar) with:

```c
    uint16_t fg, who;
    draw_block(c, x, y, w, h, state, &fg, &who);
```

In `roomsui_draw`, replace everything from `int span = ...` to the end of the
function with:

```c
    draw_grid(c, first, last, GRID_Y, GRID_B);
    int total = 0;
    for (int r = 0; r < day->nrooms; r++) {
        int x, w;
        col_span(day->nrooms, r, &x, &w);
        draw_room_head(c, &day->room[r], x, w, v);
        for (int i = 0; i < day->room[r].n; i++) {
            const room_ev_t *e = &day->room[r].ev[i];
            int y0 = y_of(e->start, first, last, GRID_Y, GRID_B) + 1, y1 = y_of(e->end, first, last, GRID_Y, GRID_B) - 1;
            if (y1 - y0 < 8) y1 = y0 + 8;
            draw_booking(c, e, x, y0, w, y1 - y0, ev_state(e, v));
            total++;
        }
    }
    if (!total) draw_empty(c, v, first, last, GRID_Y, GRID_B);
    draw_now(c, v, first, last, GRID_Y, GRID_B);
}
```

In `roomsui_hit`, replace the part from `int span = ...` to the end with:

```c
    for (int r = 0; r < day->nrooms; r++) {
        int cx, cw;
        col_span(day->nrooms, r, &cx, &cw);
        if (x < cx - COL_GAP / 2 || x >= cx + cw + COL_GAP / 2) continue;
        const room_ev_t *best = hit_in(&day->room[r], y, first, last, GRID_Y, GRID_B);
        if (best) *room = r;
        return best;
    }
    return NULL;
}
```

- [ ] **Step 4: Check that the screen is unchanged, to the byte**

```bash
cd /Users/reza/stan/tell/host_tests && make test_rooms && cd .. && ./host_tests/test_rooms
for f in /tmp/rooms-before/*.bmp; do cmp "$f" host_tests/renders/rooms/$(basename $f) || echo "DIFFERS: $f"; done
```
Expected: `rooms: all passed`, and no `DIFFERS` lines.

- [ ] **Step 5: Commit**

```bash
git add panel1/main/roomsui.c
git commit -m "panel1: the overview's grid, blocks and hits as helpers, for a room page to share"
```

---

### Task 2: A room's status line

**Files:** modify `panel1/main/roomsui.h`, `panel1/main/roomsui.c` and
`host_tests/test_rooms.c`.

- [ ] **Step 1: Write the failing test**

In `test_rooms.c`, before `canvas_t c;`, add:

```c
    /* A room's status line: taken, free, another day. */
    room_t sr = { .name = "TALL", .cap = 8, .n = 3 };
    sr.ev[0] = (room_ev_t){ .start = 600, .end = 645 };     /* 10:00-10:45 */
    sr.ev[1] = (room_ev_t){ .start = 645, .end = 660 };     /* back to back, to 11:00 */
    sr.ev[2] = (room_ev_t){ .start = 840, .end = 900 };     /* 14:00-15:00 */
    char st[64];
    roomsui_view_t sv = { .offset = 0, .wday = 4, .now = 620 };
    CHECK(roomsui_room_status(&sr, &sv, st, sizeof st) == ROOMSUI_TAKEN && strcmp(st, "Taken till 11:00, then free till 14:00") == 0);
    sv.now = 850;
    CHECK(roomsui_room_status(&sr, &sv, st, sizeof st) == ROOMSUI_TAKEN && strcmp(st, "Taken till 15:00") == 0);
    sv.now = 540;
    CHECK(roomsui_room_status(&sr, &sv, st, sizeof st) == ROOMSUI_FREE && strcmp(st, "Free till 10:00") == 0);
    sv.now = 910;
    CHECK(roomsui_room_status(&sr, &sv, st, sizeof st) == ROOMSUI_FREE && strcmp(st, "Free") == 0);
    sv = (roomsui_view_t){ .offset = 1, .wday = 5, .now = 620 };
    CHECK(roomsui_room_status(&sr, &sv, st, sizeof st) == ROOMSUI_OTHER && strcmp(st, "3 bookings") == 0);
    sr.n = 1;
    CHECK(roomsui_room_status(&sr, &sv, st, sizeof st) == ROOMSUI_OTHER && strcmp(st, "1 booking") == 0);
    sr.n = 0;
    CHECK(roomsui_room_status(&sr, &sv, st, sizeof st) == ROOMSUI_OTHER && strcmp(st, "No bookings") == 0);
```

- [ ] **Step 2: Run it to see it fail**

Run the host test. Expected: a compile error, because
`roomsui_room_status` and `ROOMSUI_TAKEN` are undeclared.

- [ ] **Step 3: Declare it in `roomsui.h`** (after `roomsui_detail`)

```c
/* What a room's status line says, into out, and its colour: ROOMSUI_TAKEN
   (red) or ROOMSUI_FREE (green) today, ROOMSUI_OTHER (grey) on another day.
     "Taken till 11:00, then free till 14:00"   "Taken till 15:00"
     "Free till 10:00"   "Free"   "3 bookings"   "No bookings"  */
enum { ROOMSUI_OTHER, ROOMSUI_FREE, ROOMSUI_TAKEN };
int roomsui_room_status(const room_t *r, const roomsui_view_t *v, char *out, size_t n);
```

- [ ] **Step 4: Implement it in `roomsui.c`** (after `taken_till`)

```c
int roomsui_room_status(const room_t *r, const roomsui_view_t *v, char *out, size_t n)
{
    if (v->offset != 0 || v->now < 0) {
        if (r->n) snprintf(out, n, "%d booking%s", r->n, r->n == 1 ? "" : "s");
        else snprintf(out, n, "No bookings");
        return ROOMSUI_OTHER;
    }
    int till = taken_till(r, v->now);
    if (till >= 0) {
        const room_ev_t *nx = rooms_next(r, till);
        if (nx) snprintf(out, n, "Taken till %d:%02d, then free till %d:%02d", till / 60, till % 60, nx->start / 60, nx->start % 60);
        else snprintf(out, n, "Taken till %d:%02d", till / 60, till % 60);
        return ROOMSUI_TAKEN;
    }
    const room_ev_t *nx = rooms_next(r, v->now);
    if (nx) snprintf(out, n, "Free till %d:%02d", nx->start / 60, nx->start % 60);
    else snprintf(out, n, "Free");
    return ROOMSUI_FREE;
}
```

- [ ] **Step 5: Run the test to see it pass.** Expected: `rooms: all passed`.

- [ ] **Step 6: Commit**

```bash
git add panel1/main/roomsui.h panel1/main/roomsui.c host_tests/test_rooms.c
git commit -m "panel1: a room's status line -- taken till when, then free till when"
```

---

### Task 3: The room page, and what a tap lands on

**Files:** modify `panel1/main/roomsui.h`, `panel1/main/roomsui.c` and
`host_tests/test_rooms.c`.

- [ ] **Step 1: Write the failing tests and renders**

In `test_rooms.c`, after the `bmp("weekend");` line (it needs `o`, the empty day parsed just above it), add:

```c
    /* The room page: CEDAR today at 13:12 (on Hiring sync's heels), and tomorrow. */
    v = (roomsui_view_t){ .offset = 0, .wday = 3, .now = 13 * 60 + 12 };
    roomsui_room_draw(&c, &d, 0, &v);
    bmp("room-today");
    v = (roomsui_view_t){ .offset = 1, .wday = 4, .now = 19 * 60 + 24 };
    roomsui_room_draw(&c, &d, 4, &v);
    bmp("room-tomorrow");
    roomsui_room_draw(&c, &d, 0, &v);
    roomsui_detail(&c, &d.room[0], &d.room[0].ev[0], &v);
    bmp("room-detail");
    v = (roomsui_view_t){ .offset = 3, .wday = 1, .now = 19 * 60 + 24 };
    roomsui_room_draw(&c, &o, 2, &v);
    bmp("room-empty");

    /* Taps: the overview's name strips, and a room page's bookings and top. */
    int nr = d.nrooms, avail = 480 - 26 - 4 - 4 * (nr - 1);
    for (int i = 0; i < nr; i++)
        CHECK(roomsui_head_hit(&d, 26 + i * (avail / nr + 4) + avail / nr / 2, 52 + 20) == i);
    CHECK(roomsui_head_hit(&d, 240, 200) == -1);                       /* the grid */
    CHECK(roomsui_head_hit(&d, 240, 20) == -1);                        /* the header */
    CHECK(roomsui_head_hit(NULL, 60, 72) == -1);
    CHECK(roomsui_room_head_hit(240, 20) && roomsui_room_head_hit(240, 80));
    CHECK(!roomsui_room_head_hit(240, 200));
    int rb = 480 - 24;                                                 /* the room page's grid bottom */
    int ry = gy + (690 - first * 60) * (rb - gy) / span;               /* 11:30, Design review's middle */
    CHECK(roomsui_room_hit(&d, 0, 240, ry) == &d.room[0].ev[0]);
    CHECK(roomsui_room_hit(&d, 0, 240, gy + (600 - first * 60) * (rb - gy) / span) == NULL);
    CHECK(roomsui_room_hit(&d, 0, 240, 30) == NULL);
    CHECK(roomsui_room_hit(&d, 9, 240, ry) == NULL);
```

(`gy`, `first` and `span` are the variables of the overview's hit test
above.)

- [ ] **Step 2: Run it to see it fail.** Expected: a compile error, because
  `roomsui_room_draw` and the other new functions are undeclared.

- [ ] **Step 3: Declare them in `roomsui.h`** (after `roomsui_room_status`)

```c
/* One room's page: its name, seats and day, a status strip, and its bookings
   full width on the same hours as the overview, a dot per room at the foot.
   With no day, or no such room, it draws the overview instead. */
void roomsui_room_draw(canvas_t *c, const rooms_day_t *day, int room, const roomsui_view_t *v);

/* The room whose name strip on the overview is under (x, y), or -1. */
int roomsui_head_hit(const rooms_day_t *day, int x, int y);

/* The booking under (x, y) on room `room`'s page, or NULL. */
const room_ev_t *roomsui_room_hit(const rooms_day_t *day, int room, int x, int y);

/* Is (x, y) in a room page's header or status strip: the way back? */
bool roomsui_room_head_hit(int x, int y);
```

- [ ] **Step 4: Implement them in `roomsui.c`**

Add near the frame's defines:

```c
#define RGRID_B  (H - 24)           /* the room page's grid ends above its dots */
```

Split `draw_head` so the room page can use it. Replace the whole function
with:

```c
static const char *day_word(const roomsui_view_t *v)
{
    return v->offset == 0 ? "Today" : v->offset == 1 ? "Tomorrow"
         : v->offset == -1 ? "Yesterday" : WDAY[v->wday % 7];
}

/* `word` large on the left, `sub` beside it on its baseline, the clock on
   the right and any note in amber before it. */
static void draw_head_with(canvas_t *c, const roomsui_view_t *v, const char *word, const char *sub_text)
{
    const aafont_t *big = &aafont_rooms_head, *sub = &aafont_inter_sub;
    int y = (HEAD_H - big->cap) / 2;
    int x = 14 + aafont_draw(c, big, 14, y, word, C_TEXT, AAFONT_LEFT) + 12;
    int base = y + big->cap;    /* the small text sits on the big word's baseline */
    if (sub_text[0]) x += aafont_draw(c, sub, x, base - sub->cap, sub_text, C_DIM, AAFONT_LEFT) + 12;

    int right = W - 14;
    if (v->now >= 0) {
        char clk[12];
        snprintf(clk, sizeof clk, "%d:%02d", v->now / 60, v->now % 60);
        right -= aafont_draw(c, big, right, y, clk, C_TEXT, AAFONT_RIGHT | AAFONT_ADVANCE) + 14;
    }
    if (v->note && v->note[0]) {
        char t[48];
        ellipsize(&aafont_inter_label, v->note, right - x, t, sizeof t);
        aafont_draw(c, &aafont_inter_label, right, base - aafont_inter_label.cap, t, C_AMBER, AAFONT_RIGHT);
    }
}

static void draw_head(canvas_t *c, const roomsui_view_t *v, const rooms_day_t *day)
{
    char date[32] = "";
    if (day) {
        if (v->offset >= -1 && v->offset <= 1)
            snprintf(date, sizeof date, "%.3s %d %s", WDAY[v->wday % 7], day->d, MON[(day->m + 11) % 12]);
        else
            snprintf(date, sizeof date, "%d %s", day->d, MON[(day->m + 11) % 12]);
    }
    draw_head_with(c, v, day_word(v), date);
}
```

Then add, after `roomsui_hit`:

```c
/* A booking on a room page: "10:00-10:45  Hiring sync", and under it, when
   there is room, "Nora Ali  -  4 invited". */
static void draw_room_booking(canvas_t *c, const room_ev_t *e, int x, int y, int w, int h, int state)
{
    uint16_t fg, who;
    draw_block(c, x, y, w, h, state, &fg, &who);
    if (h < 16) return;                  /* a sliver: the block says enough */
    const aafont_t *tf = &aafont_rooms_title, *lf = &aafont_rooms_small;
    bool two = h >= 34;
    int ty = two ? y + 6 : y + (h - tf->cap) / 2;
    char t[16];
    snprintf(t, sizeof t, "%d:%02d-%d:%02d", e->start / 60, e->start % 60, e->end / 60, e->end % 60);
    int tx = x + 9;
    tx += aafont_draw(c, lf, tx, ty + tf->cap - lf->cap, t, who, AAFONT_LEFT) + 10;
    char title[LINE_LEN];
    ellipsize(tf, e->title, x + w - 8 - tx, title, sizeof title);
    aafont_draw(c, tf, tx, ty, title, fg, AAFONT_LEFT);
    if (!two) return;

    const char *name = e->full[0] ? e->full : e->who;
    char line[80];
    if (name[0] && e->nguests) snprintf(line, sizeof line, "%s  -  %d invited", name, e->nguests);
    else if (name[0]) snprintf(line, sizeof line, "%s", name);
    else if (e->nguests) snprintf(line, sizeof line, "%d invited", e->nguests);
    else return;
    char cut[80];
    ellipsize(lf, line, w - 18, cut, sizeof cut);
    aafont_draw(c, lf, x + 9, ty + tf->cap + 9, cut, who, AAFONT_LEFT);
}

void roomsui_room_draw(canvas_t *c, const rooms_day_t *day, int room, const roomsui_view_t *v)
{
    if (!day || room < 0 || room >= day->nrooms) { roomsui_draw(c, day, v); return; }
    const room_t *r = &day->room[room];
    canvas_fill_rect(c, 0, 0, W, H, C_BG);

    char when[24], sub[48];
    if (v->offset >= -1 && v->offset <= 1) snprintf(when, sizeof when, "%s", day_word(v));
    else snprintf(when, sizeof when, "%.3s %d %s", WDAY[v->wday % 7], day->d, MON[(day->m + 11) % 12]);
    if (r->cap) snprintf(sub, sizeof sub, "%d seat%s  -  %s", r->cap, r->cap == 1 ? "" : "s", when);
    else snprintf(sub, sizeof sub, "%s", when);
    draw_head_with(c, v, r->name, sub);

    /* The status strip, the width of the grid. */
    char line[64];
    int st = roomsui_room_status(r, v, line, sizeof line);
    const int sx = GUTTER, sw = W - GUTTER - MARGIN;
    round_rect(c, sx, ROOM_Y, sw, ROOM_H, 5, st == ROOMSUI_TAKEN ? C_BUSY : st == ROOMSUI_FREE ? C_FREE : C_HEAD);
    const aafont_t *nf = &aafont_rooms_name;
    aafont_draw(c, nf, sx + sw / 2, ROOM_Y + (ROOM_H - nf->cap) / 2, line, st == ROOMSUI_OTHER ? C_DIM : C_TEXT, AAFONT_CENTRE);

    int first, last;
    roomsui_hours(day, &first, &last);
    draw_grid(c, first, last, GRID_Y, RGRID_B);
    for (int i = 0; i < r->n; i++) {
        const room_ev_t *e = &r->ev[i];
        int y0 = y_of(e->start, first, last, GRID_Y, RGRID_B) + 1, y1 = y_of(e->end, first, last, GRID_Y, RGRID_B) - 1;
        if (y1 - y0 < 8) y1 = y0 + 8;
        draw_room_booking(c, e, sx, y0, sw, y1 - y0, ev_state(e, v));
    }
    if (!r->n) draw_empty(c, v, first, last, GRID_Y, RGRID_B);
    draw_now(c, v, first, last, GRID_Y, RGRID_B);

    /* Which room: a dot each, this one lit. */
    const int pitch = 14;
    int x0 = W / 2 - (day->nrooms - 1) * pitch / 2;
    for (int i = 0; i < day->nrooms; i++) canvas_disc(c, x0 + i * pitch, H - 11, 3, i == room ? C_TEXT : C_FAINT);
}

int roomsui_head_hit(const rooms_day_t *day, int x, int y)
{
    if (!day || y < ROOM_Y || y >= GRID_Y) return -1;
    for (int r = 0; r < day->nrooms; r++) {
        int cx, cw;
        col_span(day->nrooms, r, &cx, &cw);
        if (x >= cx - COL_GAP / 2 && x < cx + cw + COL_GAP / 2) return r;
    }
    return -1;
}

const room_ev_t *roomsui_room_hit(const rooms_day_t *day, int room, int x, int y)
{
    (void)x;
    if (!day || room < 0 || room >= day->nrooms || y < GRID_Y) return NULL;
    int first, last;
    roomsui_hours(day, &first, &last);
    return hit_in(&day->room[room], y, first, last, GRID_Y, RGRID_B);
}

bool roomsui_room_head_hit(int x, int y)
{
    (void)x;
    return y < ROOM_Y + ROOM_H;
}
```

- [ ] **Step 5: Run the tests, and look at the four new renders**

Expected: `rooms: all passed`, and Task 1's baseline renders still
byte-identical. Convert the room renders to PNG and look at them:

```bash
for f in room-today room-tomorrow room-detail room-empty; do sips -s format png host_tests/renders/rooms/$f.bmp --out /tmp/$f.png >/dev/null; done
```

Check:
- `room-today`: CEDAR's strip is red and reads "Taken till 14:00". Budget
  planning (13:00-13:30) runs straight on into Ops weekly (13:30-14:00), and
  nothing follows. The 11:00 block has two lines, the 15-minute block one
  line, and the red line is at 13:12.
- `room-tomorrow`: BIRCH, a grey strip "3 bookings", no now-line, and five
  dots with the fifth lit.
- Nothing overlaps the dots.

- [ ] **Step 6: Commit**

```bash
git add panel1/main/roomsui.h panel1/main/roomsui.c host_tests/test_rooms.c
git commit -m "panel1: a room's own page -- full-width bookings, its status strip, a dot per room"
```

---

### Task 4: Retire the screen saver's drawing

**Files:** modify `panel1/main/roomsui.h`, `panel1/main/roomsui.c`,
`panel1/main/fonts.c`, `host_tests/test_rooms.c` and `panel1/main/panel1.c`
(only its saver call); delete `panel1/main/font_rooms_clock.h`.

- [ ] **Step 1:** Delete `roomsui_saver` and its comment from `roomsui.h`.
  Delete the function from `roomsui.c`, and the
  `extern const aafont_t aafont_rooms_clock;` line. In `fonts.c`, delete
  `#include "font_rooms_clock.h"`. Then
  `git rm panel1/main/font_rooms_clock.h`.
- [ ] **Step 2:** In `test_rooms.c`, delete the four lines from
  `roomsui_saver(&c, 7 * 60 + 42 ...` to `bmp("saver-1");`. Delete the old
  renders: `rm -f host_tests/renders/rooms/saver-*.bmp`.
- [ ] **Step 3:** In `panel1.c`'s `draw()`, delete the block
  `if (s_saver && v.now >= 0) { ... return; }`. Task 6 replaces what it did.
- [ ] **Step 4:** Run the host test. Expected: `rooms: all passed`.
- [ ] **Step 5: Commit**

```bash
git add -A panel1/main/roomsui.h panel1/main/roomsui.c panel1/main/fonts.c panel1/main/font_rooms_clock.h host_tests/test_rooms.c panel1/main/panel1.c
git commit -m "panel1: the wandering-clock saver goes; sleep takes its place"
```

---

### Task 5: Room navigation in panel1.c

**Files:** modify `panel1/main/panel1.c`.

- [ ] **Step 1: State, and a way home**

Next to `static volatile int s_offset;`, add:

```c
static volatile int s_room = -1;      /* the open room's page; -1 the overview */
```

After `home_offset()`, add:

```c
/* The overview on the home day, no card open. */
static void go_home(void)
{
    if (net_time_ok()) s_offset = home_offset();
    s_room = -1;
    s_det = false;
    s_dirty = true;
}
```

(`s_det` is declared below `s_offset`. Move the `s_det` block of statics up
above `home_offset` if the compiler complains.)

- [ ] **Step 2: Draw the room page**

In `draw()`, replace `roomsui_draw(c, day, &v);` with:

```c
    if (s_room >= 0 && day && s_room >= day->nrooms) s_room = -1;   /* that day has fewer rooms */
    roomsui_room_draw(c, day, s_room, &v);
```

`roomsui_room_draw` draws the overview when `s_room` is -1 or there is no day
yet.

- [ ] **Step 3: Vertical swipes, and taps on the strips**

In `app_main`, change `int x0 = 0, y0 = 0, lx = 0;` to
`int x0 = 0, y0 = 0, lx = 0, ly = 0;`. In the touch branch, replace

```c
            lx = x;
            if (abs(x - x0) > SWIPE_PX / 2) moved = true;
```
with
```c
            lx = x; ly = y;
            if (abs(x - x0) > SWIPE_PX / 2 || abs(y - y0) > SWIPE_PX / 2) moved = true;
```

Replace the whole release branch, from `} else if (down) {` up to (not
including) the blank line before `if (net_time_ok()) {`, with:

```c
        } else if (down) {
            down = false;
            int dx = lx - x0, dy = ly - y0;
            if (moved && abs(dx) >= SWIPE_PX && abs(dx) >= abs(dy)) {
                /* A finger dragged left pulls the next day in, as a page turns. */
                int step = dx < 0 ? 1 : -1, off = s_offset + step;
                while (weekend(off)) off += step;       /* Saturday and Sunday are skipped */
                if (off >= -SPAN && off <= SPAN) { s_offset = off; s_dirty = true; }
                s_det = false;
            } else if (moved && abs(dy) >= SWIPE_PX && s_room >= 0 && s_buf_ok && buf->nrooms > 0) {
                /* Dragged up, the next room comes in; round from the last to the first. */
                int n = buf->nrooms;
                s_room = (s_room + (dy < 0 ? 1 : n - 1)) % n;
                s_det = false;
                s_dirty = true;
            } else if (!moved && s_det) {
                /* Any tap closes a booking's card. */
                s_det = false;
                s_dirty = true;
            } else if (!moved && s_room >= 0) {
                /* A room page: its top goes back to the overview, a booking opens its card. */
                const room_ev_t *e = s_buf_ok ? roomsui_room_hit(buf, s_room, x0, y0) : NULL;
                if (roomsui_room_head_hit(x0, y0)) {
                    s_room = -1;
                    s_dirty = true;
                } else if (e) {
                    *s_det_room = buf->room[s_room];
                    s_det_ev = *e;
                    s_det = true;
                    s_det_at = now_s();
                    s_dirty = true;
                }
            } else if (!moved) {
                /* The overview: a room's name opens its page, a booking its
                   card; anywhere else goes home. */
                int room, r = s_buf_ok ? roomsui_head_hit(buf, x0, y0) : -1;
                const room_ev_t *e = s_buf_ok ? roomsui_hit(buf, x0, y0, &room) : NULL;
                if (r >= 0) {
                    s_room = r;
                    s_dirty = true;
                } else if (e) {
                    *s_det_room = buf->room[room];
                    s_det_ev = *e;
                    s_det = true;
                    s_det_at = now_s();
                    s_dirty = true;
                } else {
                    int h = home_offset();
                    if (s_offset != h) { s_offset = h; s_dirty = true; }
                }
            }
        }
```

- [ ] **Step 4: Going home after a minute untouched also leaves a room page**

Replace

```c
            if (s_offset != h && now_s() - last_touch >= IDLE_HOME && !down) { s_offset = h; s_det = false; s_dirty = true; }
```
with
```c
            if ((s_offset != h || s_room >= 0) && now_s() - last_touch >= IDLE_HOME && !down) go_home();
```

- [ ] **Step 5: Update the header comment.** Replace the sentence "A tap on
  a booking opens a card with all of it -- who booked it, who is invited."
  with:

```
 * A tap on a
 * booking opens a card with all of it -- who booked it, who is invited -- and
 * a tap on a room's name opens that room's own page: swipe up and down there
 * for the other rooms, and tap its top to come back.
```
  Rewrap the paragraph to 80 columns.

- [ ] **Step 6: Build.** Run the firmware build. Expected:
  `Project build complete`, or the esptool hint lines that follow a good
  build.
- [ ] **Step 7: Commit**

```bash
git add panel1/main/panel1.c
git commit -m "panel1: a room's name opens its page; swipe up and down for the others"
```

---

### Task 6: Sleep, and the buttons that wake it

**Files:** modify `panel1/main/lcd.h`, `panel1/main/lcd.c` and
`panel1/main/panel1.c`.

- [ ] **Step 1: Read the PWR key (`lcd.c`, after `tca_set`)**

```c
#define TCA_IN   0x00
/* PWR pulls EXIO4 low while it is held: the AXP2101's PWRON line, pulled up.
   Checked on the board (Task 7); flip this if the log says otherwise. */
#define KEY_DOWN 0

bool lcd_pwr_key(void)
{
    uint8_t reg = TCA_IN, v = 0;
    if (i2c_master_transmit_receive(s_tca, &reg, 1, &v, 1, 50) != ESP_OK) return false;
    return ((v & X_KEY) ? 1 : 0) == KEY_DOWN;
}
```

In `lcd.h`, add `#include <stdbool.h>` and
`bool lcd_pwr_key(void);   /* true while the side PWR key is held */`.

- [ ] **Step 2: The sleep state (`panel1.c`)**

Replace `#define SAVER_AFTER ...` and `#define SAVER_LIGHT ...` with:

```c
#define SLEEP_AFTER    (5 * 60)     /* s untouched before the screen goes dark */
#define PIN_BOOT       0            /* the side BOOT key, low while held */
```

Rename `static bool s_saver;` to `static bool s_asleep;`. Add
`#include "driver/gpio.h"`.

Add, after `draw()`:

```c
/* Dark: the frame black, then the backlight off, so nothing glows through. */
static void sleep_now(canvas_t *c)
{
    s_asleep = true;
    s_det = false;
    canvas_fill_rect(c, 0, 0, c->w, c->h, 0);
    lcd_show();
    lcd_backlight(0);
    ESP_LOGI(TAG, "asleep");
}

/* Back on the overview, home day, at full light. */
static void wake(canvas_t *c, rooms_day_t *buf)
{
    s_asleep = false;
    go_home();
    draw(c, buf);
    s_dirty = false;
    lcd_backlight(100);
    ESP_LOGI(TAG, "awake");
}
```

(Check the field names in `main/canvas.h`. If the canvas has no `w`/`h`, use
`480, 480`.)

- [ ] **Step 3: The BOOT key's pin, in `app_main` before the loop**

```c
    gpio_config_t boot = { .pin_bit_mask = 1ULL << PIN_BOOT, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&boot);
    bool keys_were = false, pwr_was = false;
```

- [ ] **Step 4: The loop**

In the touch branch, replace the `if (s_saver) { ... }` block with:

```c
                if (s_asleep) {
                    /* This touch only wakes: nothing else till the finger lifts. */
                    waking = true;
                    wake(c, buf);
                }
```

Right after `vTaskDelay(pdMS_TO_TICKS(30));` at the top of the loop, add:

```c
        /* The side keys: a press wakes it, or, awake, goes home. Taken on the
           press, not the release. */
        bool pwr = lcd_pwr_key();
        if (pwr != pwr_was) { ESP_LOGI(TAG, "PWR key %s", pwr ? "down" : "up"); pwr_was = pwr; }
        bool keys = gpio_get_level(PIN_BOOT) == 0 || pwr;
        if (keys && !keys_were) {
            if (s_asleep) wake(c, buf);
            else go_home();
            last_touch = now_s();
        }
        keys_were = keys;
```

Replace the saver's start:

```c
            if (!s_saver && !down && touch && now_s() - last_touch >= SAVER_AFTER) {
                s_saver = true;
                s_det = false;
                s_dirty = true;
                lcd_backlight(SAVER_LIGHT);
            }
```
with
```c
            if (!s_asleep && !down && now_s() - last_touch >= SLEEP_AFTER) sleep_now(c);
```

and the final draw:

```c
        if (s_dirty && !down) {
```
with
```c
        if (s_dirty && !down && !s_asleep) {
```

- [ ] **Step 5: The header comment.** Replace "Five minutes untouched, it
  dims to a clock that wanders the screen; a tap wakes it on the home day."
  with "Five minutes untouched, the screen goes dark; BOOT, PWR or a tap wakes
  it on the home day."
- [ ] **Step 6: Build.** Expected: a good build.
- [ ] **Step 7: Commit**

```bash
git add panel1/main/lcd.h panel1/main/lcd.c panel1/main/panel1.c
git commit -m "panel1: dark after 5 min untouched; BOOT, PWR or a tap wakes it home"
```

---

### Task 7: On the board

- [ ] **Step 1:** Run `git fetch`, and check that `git status -sb` shows no
  `behind`. Push, then flash with `tools/flash-panel1.sh`.
- [ ] **Step 2: Check PWR's level.** Watch the log (`tools/panel1.py` does
  not show it; use a pyserial read with DTR and RTS held, as in
  `tools/panel1.py`). Ask Reza to press and release PWR once, briefly. The
  log should show "PWR key down" then "PWR key up". If it shows up then down,
  set `KEY_DOWN 1` in `lcd.c`, then rebuild, flash and check again. If
  nothing shows, EXIO4 is not the key: stop and report.
- [ ] **Step 3: Rooms.** Ask Reza to check each of these:
  - Tap each room's name: its page opens.
  - Swipe up and down through all six rooms and round.
  - Swipe days on a room page.
  - Tap the top of the page to come back.
  - Leave it for a minute: it goes home.
- [ ] **Step 4: Sleep.** The log shows "asleep" after 5 minutes untouched.
  Then BOOT, then PWR, then a tap each wake it ("awake"), and each wake lands
  on the overview with nothing opened.
- [ ] **Step 5:** Fix the spec's separators so they match the screen
  (ASCII). Commit, push, and move the kanban card to done.
