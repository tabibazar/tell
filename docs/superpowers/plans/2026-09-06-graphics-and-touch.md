# Graphics and Touch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the CrowPanel colour charts of Claude usage and a clock, cycled by tapping the screen.

**Architecture:** The firmware owns all drawing; the Mac sends data, not pixels. `usagedata` parses marker-prefixed text into structs, `pages` decides what is showing, `canvas` draws it with colour and filled rectangles, `gt911` polls the touch controller. Three of the five units are pure C and host-tested.

**Tech Stack:** ESP-IDF 5.5.5, NimBLE, esp_lcd, C11; Python 3 for the sender; Swift client unchanged.

**Spec:** `docs/superpowers/specs/2026-09-06-graphics-and-touch-design.md`

## Global Constraints

- Build: `. tools/idf-env.sh` first. Small board `idf.py build`; big board `idf.py -B build-crowpanel -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.crowpanel" -D SDKCONFIG=sdkconfig.crowpanel build`.
- Flash small: `write_flash 0x10000 build/screen.bin` on `/dev/cu.usbmodem*`. Flash big: `write_flash 0x10000 build-crowpanel/screen.bin` on `/dev/cu.usbserial-*`.
- **Never pass `--baud 921600`** (or 460800) to esptool: both boards fail. Default rate only.
- **Check `build/screen.bin`'s timestamp before flashing.** A failed build leaves the previous binary in place.
- **Check the image size changes when it should.** A silent config failure once compiled the wrong font and ran perfectly.
- Board names: `big` (CrowPanel), `small` (Feather). Test with `tell --device big "..."`.
- GT911: I²C **SCL 20, SDA 19**, no INT/RST. Address `0x5D` or `0x14`.
- Panel 800×480, 64×20 text cells at 12×24. Feather 240×135, 20×5.
- Message cap **2048 bytes**; reassembly flushes after **250 ms** idle.
- Font is ASCII 32..126 only.

---

### Task 1: Colour and rectangles in `canvas`

**Files:**
- Create: `main/palette.h`
- Modify: `main/canvas.h`, `main/canvas.c`
- Test: `host_tests/test_canvas.c`, `host_tests/Makefile`

**Interfaces:**
- Produces: `void canvas_fill_rect(canvas_t *c, int x, int y, int w, int h, uint16_t colour)` and `void canvas_puts(canvas_t *c, int col, int row, const char *s, uint16_t colour)`. Existing `canvas_text` and `canvas_big` keep their signatures and draw in `PAL_FG`.

- [ ] **Step 1: Write the failing tests** — append to `host_tests/test_canvas.c`, inside `main()` before the pass/fail report:

```c
    /* Rectangles must clamp, not write outside the framebuffer. */
    canvas_init(&small, small_fb, 240, 135, 1);
    canvas_clear(&small);
    canvas_fill_rect(&small, 0, 0, 10, 10, 0xF800);
    expect("fill_rect paints its area", lit_colour(&small, 0xF800) == 100);

    canvas_clear(&small);
    canvas_fill_rect(&small, -5, -5, 10, 10, 0xF800);
    expect("negative origin is clipped", lit_colour(&small, 0xF800) == 25);

    canvas_clear(&small);
    canvas_fill_rect(&small, 235, 130, 100, 100, 0xF800);
    expect("oversize is clipped to the panel", lit_colour(&small, 0xF800) == 25);

    canvas_clear(&small);
    canvas_fill_rect(&small, 0, 0, 0, 0, 0xF800);
    expect("zero size paints nothing", lit_colour(&small, 0xF800) == 0);

    canvas_clear(&small);
    canvas_puts(&small, 0, 0, "A", 0x07E0);
    expect("puts uses the colour given", lit_colour(&small, 0x07E0) > 0);
    expect("puts does not paint white", lit_colour(&small, 0xFFFF) == 0);
```

Add this helper above `main()`:

```c
static int lit_colour(canvas_t *c, uint16_t colour)
{
    int n = 0;
    for (int i = 0; i < c->w * c->h; i++) if (c->fb[i] == colour) n++;
    return n;
}
```

- [ ] **Step 2: Run and confirm it fails**

Run: `make -C host_tests test_canvas`
Expected: FAIL — `canvas_fill_rect` and `canvas_puts` undeclared.

- [ ] **Step 3: Create `main/palette.h`**

```c
#ifndef PALETTE_H
#define PALETTE_H

/* RGB565. One palette so pages cannot drift apart. */
#define PAL_BG       0x0000  /* black */
#define PAL_FG       0xFFFF  /* white */
#define PAL_DIM      0x8410  /* grey, for labels and axes */
#define PAL_TITLE_BG 0x001F  /* blue title bar */

/* Accent colours, assigned to models in rank order. */
#define PAL_A0       0x07FF  /* cyan */
#define PAL_A1       0xF81F  /* magenta */
#define PAL_A2       0xFFE0  /* yellow */
#define PAL_A3       0x07E0  /* green */

static inline unsigned short pal_accent(int i)
{
    const unsigned short a[4] = { PAL_A0, PAL_A1, PAL_A2, PAL_A3 };
    return a[i & 3];
}

#endif /* PALETTE_H */
```

- [ ] **Step 4: Add the declarations to `main/canvas.h`**

```c
/* Filled rectangle in framebuffer pixels. Clipped to the panel. */
void canvas_fill_rect(canvas_t *c, int x, int y, int w, int h, uint16_t colour);

/* One line of text at a character cell, in the given colour. Not wrapped;
   clipped at the right edge. */
void canvas_puts(canvas_t *c, int col, int row, const char *s, uint16_t colour);
```

- [ ] **Step 5: Implement in `main/canvas.c`**

Change the private `glyph()` to take a colour and use it instead of `CANVAS_FG`:

```c
static void glyph(canvas_t *c, char ch, int ox, int oy, int scale, uint16_t colour)
```

replacing `c->fb[py * c->w + px] = CANVAS_FG;` with `= colour;`, and update its
existing callers in `canvas_text` and `canvas_big` to pass `CANVAS_FG`. Then add:

```c
void canvas_fill_rect(canvas_t *c, int x, int y, int w, int h, uint16_t colour)
{
    if (w <= 0 || h <= 0) return;

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > c->w ? c->w : x + w;
    int y1 = y + h > c->h ? c->h : y + h;

    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++)
            c->fb[py * c->w + px] = colour;
}

void canvas_puts(canvas_t *c, int col, int row, const char *s, uint16_t colour)
{
    if (s == NULL) return;
    int cell_w = FONT_W * c->scale;
    int cell_h = FONT_H * c->scale;
    for (int i = 0; s[i] != '\0'; i++) {
        int x = (col + i) * cell_w;
        if (x >= c->w) return;
        glyph(c, s[i], x, row * cell_h, c->scale, colour);
    }
}
```

- [ ] **Step 6: Run and confirm it passes**

Run: `make -C host_tests test_canvas && ./host_tests/test_canvas`
Expected: `all tests passed`

- [ ] **Step 7: Commit**

```bash
git add main/palette.h main/canvas.h main/canvas.c host_tests/test_canvas.c
git commit -m "Add colour and filled rectangles to canvas"
```

---

### Task 2: `usagedata` — parse the wire format

**Files:**
- Create: `main/usagedata.h`, `main/usagedata.c`
- Test: `host_tests/test_usagedata.c`
- Modify: `host_tests/Makefile`, `.gitignore`

**Interfaces:**
- Produces: `ud_kind_t usagedata_parse(usagedata_t *d, const char *payload)` returning `UD_NONE`, `UD_STATS` or `UD_DAILY`; structs `usagedata_t`, `ud_model_t`, `ud_day_t`.

- [ ] **Step 1: Write the failing test** — create `host_tests/test_usagedata.c`:

```c
#include "usagedata.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

int main(void)
{
    usagedata_t d;
    memset(&d, 0, sizeof d);

    expect("plain text is not data", usagedata_parse(&d, "hello world") == UD_NONE);
    expect("unknown marker is not data", usagedata_parse(&d, "!nope\nx 1") == UD_NONE);
    expect("NULL is not data", usagedata_parse(&d, NULL) == UD_NONE);

    expect("stats marker recognised",
           usagedata_parse(&d, "!stats\nm opus-5 13000000 3300000000\n"
                               "m sonnet-5 1200000 429000000\n") == UD_STATS);
    expect("two models parsed", d.model_count == 2);
    expect("model name kept", strcmp(d.models[0].name, "opus-5") == 0);
    expect("output tokens parsed", d.models[0].out == 13000000ULL);
    expect("cache tokens exceed 32 bits", d.models[0].cread == 3300000000ULL);

    expect("daily marker recognised",
           usagedata_parse(&d, "!daily\nd 09-01 571000000\nd 09-02 282000000\n") == UD_DAILY);
    expect("two days parsed", d.day_count == 2);
    expect("day label kept", strcmp(d.days[0].label, "09-01") == 0);
    expect("day tokens parsed", d.days[1].tokens == 282000000ULL);

    expect("stats survived the daily parse", d.model_count == 2);

    usagedata_parse(&d, "!stats\nm a 1 2\ngarbage line\nm b 3 4\n");
    expect("a bad line does not lose good rows", d.model_count == 2);

    usagedata_parse(&d, "!stats\nm only-name\n");
    expect("a row missing fields is skipped", d.model_count == 0);

    usagedata_parse(&d, "!stats\nm trunc 5");
    expect("a payload truncated mid-line still parses", d.model_count == 1);

    char many[1024] = "!stats\n";
    for (int i = 0; i < UD_MAX_MODELS + 4; i++) {
        char row[40];
        snprintf(row, sizeof row, "m mdl%d %d %d\n", i, i, i);
        strncat(many, row, sizeof many - strlen(many) - 1);
    }
    usagedata_parse(&d, many);
    expect("model count is capped", d.model_count == UD_MAX_MODELS);

    usagedata_parse(&d, "!stats\nm verylongmodelnamethatoverflows 1 2\n");
    expect("long name is truncated, not overflowed",
           strlen(d.models[0].name) <= UD_NAME_MAX);

    usagedata_parse(&d, "!stats\n");
    expect("empty section yields no rows", d.model_count == 0);

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
```

- [ ] **Step 2: Run and confirm it fails**

Run: `make -C host_tests test_usagedata`
Expected: FAIL — `usagedata.h` not found.

- [ ] **Step 3: Create `main/usagedata.h`**

```c
#ifndef USAGEDATA_H
#define USAGEDATA_H

#include <stdint.h>

#define UD_MAX_MODELS 8
#define UD_MAX_DAYS   14
#define UD_NAME_MAX   15
#define UD_LABEL_MAX  7

typedef struct {
    char name[UD_NAME_MAX + 1];
    uint64_t out;
    uint64_t cread;
} ud_model_t;

typedef struct {
    char label[UD_LABEL_MAX + 1];
    uint64_t tokens;
} ud_day_t;

typedef struct {
    ud_model_t models[UD_MAX_MODELS];
    int model_count;
    ud_day_t days[UD_MAX_DAYS];
    int day_count;
} usagedata_t;

typedef enum { UD_NONE = 0, UD_STATS, UD_DAILY } ud_kind_t;

/* Parses one payload. A payload starting with "!stats" or "!daily" replaces
   that section wholesale; anything else returns UD_NONE and leaves `d`
   untouched, so the caller can treat it as a text message. */
ud_kind_t usagedata_parse(usagedata_t *d, const char *payload);

#endif /* USAGEDATA_H */
```

- [ ] **Step 4: Create `main/usagedata.c`**

```c
#include "usagedata.h"

#include <stdlib.h>
#include <string.h>

/* Copies up to `max` chars of a whitespace-delimited token. */
static const char *token(const char *p, char *out, int max)
{
    while (*p == ' ' || *p == '\t') p++;
    int n = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        if (n < max) out[n++] = *p;
        p++;
    }
    out[n] = '\0';
    return n ? p : NULL;
}

static const char *next_line(const char *p)
{
    while (*p && *p != '\n') p++;
    return *p ? p + 1 : p;
}

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

ud_kind_t usagedata_parse(usagedata_t *d, const char *payload)
{
    if (d == NULL || payload == NULL) return UD_NONE;

    ud_kind_t kind;
    if (starts_with(payload, "!stats")) kind = UD_STATS;
    else if (starts_with(payload, "!daily")) kind = UD_DAILY;
    else return UD_NONE;

    /* A section replaces its rows wholesale: no merge, so no stale rows. */
    if (kind == UD_STATS) d->model_count = 0;
    else d->day_count = 0;

    for (const char *p = next_line(payload); *p; p = next_line(p)) {
        char tag[4];
        const char *q = token(p, tag, 3);
        if (q == NULL) continue;

        if (kind == UD_STATS && tag[0] == 'm' && tag[1] == '\0') {
            if (d->model_count >= UD_MAX_MODELS) continue;
            ud_model_t m;
            char num[24];
            q = token(q, m.name, UD_NAME_MAX);
            if (q == NULL) continue;
            q = token(q, num, sizeof num - 1);
            if (q == NULL) continue;
            m.out = strtoull(num, NULL, 10);
            q = token(q, num, sizeof num - 1);
            m.cread = (q == NULL) ? 0 : strtoull(num, NULL, 10);
            d->models[d->model_count++] = m;
        } else if (kind == UD_DAILY && tag[0] == 'd' && tag[1] == '\0') {
            if (d->day_count >= UD_MAX_DAYS) continue;
            ud_day_t day;
            char num[24];
            q = token(q, day.label, UD_LABEL_MAX);
            if (q == NULL) continue;
            q = token(q, num, sizeof num - 1);
            if (q == NULL) continue;
            day.tokens = strtoull(num, NULL, 10);
            d->days[d->day_count++] = day;
        }
    }
    return kind;
}
```

- [ ] **Step 5: Add to the Makefile** — in `host_tests/Makefile`, add `test_usagedata` to the `all:` list and the `clean:` list, and add the rule:

```make
test_usagedata: test_usagedata.c ../main/usagedata.c
	cc $(CFLAGS) -o $@ $^
```

Then add `host_tests/test_usagedata` to `.gitignore`.

- [ ] **Step 6: Run and confirm it passes**

Run: `make -C host_tests test_usagedata && ./host_tests/test_usagedata`
Expected: `all tests passed`

Note: `m only-name` must yield `model_count == 0` — the row is abandoned before
the count is incremented, which is why the increment is the last statement.

- [ ] **Step 7: Commit**

```bash
git add main/usagedata.h main/usagedata.c host_tests/test_usagedata.c host_tests/Makefile .gitignore
git commit -m "Parse the usage data wire format"
```

---

### Task 3: `pages` — the page state machine

**Files:**
- Create: `main/pages.h`, `main/pages.c`
- Test: `host_tests/test_pages.c`
- Modify: `host_tests/Makefile`, `.gitignore`

**Interfaces:**
- Produces: `page_t`, `pages_t`, `pages_init`, `pages_advance`, `pages_show`, `pages_idle_expired`.

- [ ] **Step 1: Write the failing test** — create `host_tests/test_pages.c`:

```c
#include "pages.h"

#include <stdio.h>

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

#define ALL (PAGE_BIT(PAGE_CLOCK) | PAGE_BIT(PAGE_STATS) \
             | PAGE_BIT(PAGE_DAILY) | PAGE_BIT(PAGE_MESSAGE))
#define FEATHER (PAGE_BIT(PAGE_CLOCK) | PAGE_BIT(PAGE_MESSAGE))

int main(void)
{
    pages_t p;

    pages_init(&p, ALL);
    expect("starts on the clock", p.current == PAGE_CLOCK);
    expect("advance goes to stats", pages_advance(&p, 1000) == PAGE_STATS);
    expect("then daily", pages_advance(&p, 2000) == PAGE_DAILY);
    expect("then message", pages_advance(&p, 3000) == PAGE_MESSAGE);
    expect("then wraps to clock", pages_advance(&p, 4000) == PAGE_CLOCK);

    pages_init(&p, FEATHER);
    expect("feather starts on the clock", p.current == PAGE_CLOCK);
    expect("feather skips absent pages", pages_advance(&p, 1000) == PAGE_MESSAGE);
    expect("feather wraps", pages_advance(&p, 2000) == PAGE_CLOCK);

    pages_init(&p, PAGE_BIT(PAGE_CLOCK));
    expect("a lone page stays put", pages_advance(&p, 1000) == PAGE_CLOCK);

    pages_init(&p, ALL);
    pages_show(&p, PAGE_DAILY, 5000);
    expect("show jumps to a page", p.current == PAGE_DAILY);
    pages_show(&p, PAGE_STATS, 6000);
    expect("show ignores an unavailable page",
           (pages_init(&p, FEATHER), pages_show(&p, PAGE_STATS, 7000),
            p.current == PAGE_CLOCK));

    pages_init(&p, ALL);
    pages_show(&p, PAGE_STATS, 0);
    expect("not idle immediately", !pages_idle_expired(&p, 1000));
    expect("not idle just under the limit",
           !pages_idle_expired(&p, PAGES_IDLE_US - 1));
    expect("idle at the limit", pages_idle_expired(&p, PAGES_IDLE_US + 1));
    pages_advance(&p, PAGES_IDLE_US + 1);
    expect("activity resets the idle timer",
           !pages_idle_expired(&p, PAGES_IDLE_US + 2));

    pages_init(&p, ALL);
    expect("the clock is never idle", !pages_idle_expired(&p, PAGES_IDLE_US * 10));

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
```

- [ ] **Step 2: Run and confirm it fails**

Run: `make -C host_tests test_pages`
Expected: FAIL — `pages.h` not found.

- [ ] **Step 3: Create `main/pages.h`**

```c
#ifndef PAGES_H
#define PAGES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PAGE_CLOCK = 0,
    PAGE_STATS,
    PAGE_DAILY,
    PAGE_MESSAGE,
    PAGE_COUNT
} page_t;

#define PAGE_BIT(p) (1u << (p))

/* A page other than the clock reverts to the clock after this long with no
   touch and no new data. */
#define PAGES_IDLE_US (5 * 60 * 1000000LL)

typedef struct {
    unsigned available;      /* bitmask of PAGE_BIT(...) */
    page_t current;
    int64_t last_activity_us;
} pages_t;

void pages_init(pages_t *p, unsigned available);

/* Moves to the next available page and returns it. */
page_t pages_advance(pages_t *p, int64_t now_us);

/* Jumps to a page, ignored if that page is not available on this board. */
void pages_show(pages_t *p, page_t page, int64_t now_us);

/* True when the clock should take over. Always false on the clock itself. */
bool pages_idle_expired(const pages_t *p, int64_t now_us);

#endif /* PAGES_H */
```

- [ ] **Step 4: Create `main/pages.c`**

```c
#include "pages.h"

void pages_init(pages_t *p, unsigned available)
{
    p->available = available | PAGE_BIT(PAGE_CLOCK);  /* the clock always exists */
    p->current = PAGE_CLOCK;
    p->last_activity_us = 0;
}

page_t pages_advance(pages_t *p, int64_t now_us)
{
    for (int i = 1; i <= PAGE_COUNT; i++) {
        page_t candidate = (page_t)(((int)p->current + i) % PAGE_COUNT);
        if (p->available & PAGE_BIT(candidate)) {
            p->current = candidate;
            break;
        }
    }
    p->last_activity_us = now_us;
    return p->current;
}

void pages_show(pages_t *p, page_t page, int64_t now_us)
{
    if (!(p->available & PAGE_BIT(page))) return;
    p->current = page;
    p->last_activity_us = now_us;
}

bool pages_idle_expired(const pages_t *p, int64_t now_us)
{
    if (p->current == PAGE_CLOCK) return false;
    return now_us - p->last_activity_us > PAGES_IDLE_US;
}
```

- [ ] **Step 5: Add to the Makefile and `.gitignore`**

```make
test_pages: test_pages.c ../main/pages.c
	cc $(CFLAGS) -o $@ $^
```

- [ ] **Step 6: Run and confirm it passes**

Run: `make -C host_tests test_pages && ./host_tests/test_pages`
Expected: `all tests passed`

- [ ] **Step 7: Commit**

```bash
git add main/pages.h main/pages.c host_tests/test_pages.c host_tests/Makefile .gitignore
git commit -m "Add the page state machine"
```

---

### Task 4: Draw the stats and daily pages

**Files:**
- Create: `main/views.h`, `main/views.c`
- Modify: `main/CMakeLists.txt`

**Interfaces:**
- Consumes: `canvas_fill_rect`, `canvas_puts`, `pal_accent`, `usagedata_t`.
- Produces: `void views_stats(canvas_t *c, const usagedata_t *d)` and `void views_daily(canvas_t *c, const usagedata_t *d)`.

- [ ] **Step 1: Create `main/views.h`**

```c
#ifndef VIEWS_H
#define VIEWS_H

#include "canvas.h"
#include "usagedata.h"

/* Each fills the canvas; the caller blits. */
void views_stats(canvas_t *c, const usagedata_t *d);
void views_daily(canvas_t *c, const usagedata_t *d);

#endif /* VIEWS_H */
```

- [ ] **Step 2: Create `main/views.c`**

```c
#include "views.h"

#include "palette.h"

#include <stdio.h>

/* Compact magnitude: a billion has to fit in a narrow column. */
static void human(uint64_t n, char *out, int size)
{
    if (n >= 1000000000ULL) snprintf(out, size, "%.1fB", n / 1e9);
    else if (n >= 1000000ULL) snprintf(out, size, "%.1fM", n / 1e6);
    else if (n >= 1000ULL) snprintf(out, size, "%.1fK", n / 1e3);
    else snprintf(out, size, "%llu", (unsigned long long)n);
}

static void title(canvas_t *c, const char *text)
{
    int cell_h = FONT_H * c->scale;
    canvas_fill_rect(c, 0, 0, c->w, cell_h, PAL_TITLE_BG);
    canvas_puts(c, 1, 0, text, PAL_FG);
}

void views_stats(canvas_t *c, const usagedata_t *d)
{
    canvas_clear(c);
    title(c, "USAGE BY MODEL");

    if (d->model_count == 0) {
        canvas_puts(c, 1, 2, "no data yet", PAL_DIM);
        return;
    }

    uint64_t peak = 1;
    for (int i = 0; i < d->model_count; i++)
        if (d->models[i].cread + d->models[i].out > peak)
            peak = d->models[i].cread + d->models[i].out;

    int cell_w = FONT_W * c->scale;
    int cell_h = FONT_H * c->scale;
    int bar_x = 16 * cell_w;
    int bar_max = c->w - bar_x - cell_w;

    for (int i = 0; i < d->model_count && i + 2 < c->rows; i++) {
        int row = i + 2;
        char value[16];
        canvas_puts(c, 1, row, d->models[i].name, PAL_FG);

        uint64_t total = d->models[i].cread + d->models[i].out;
        int width = (int)((double)total / (double)peak * bar_max);
        if (width < 2 && total > 0) width = 2;
        canvas_fill_rect(c, bar_x, row * cell_h + cell_h / 4,
                         width, cell_h / 2, pal_accent(i));

        human(total, value, sizeof value);
        canvas_puts(c, 1, row, "", PAL_FG);           /* keep row alignment */
        canvas_puts(c, c->cols - 7, row, value, PAL_DIM);
    }
}

void views_daily(canvas_t *c, const usagedata_t *d)
{
    canvas_clear(c);
    title(c, "TOKENS PER DAY");

    if (d->day_count == 0) {
        canvas_puts(c, 1, 2, "no data yet", PAL_DIM);
        return;
    }

    uint64_t peak = 1;
    for (int i = 0; i < d->day_count; i++)
        if (d->days[i].tokens > peak) peak = d->days[i].tokens;

    int cell_h = FONT_H * c->scale;
    int top = 2 * cell_h;
    int bottom = c->h - cell_h;          /* leave a row for labels */
    int plot_h = bottom - top;
    int slot = c->w / d->day_count;
    int bar_w = slot > 4 ? slot - 4 : slot;

    for (int i = 0; i < d->day_count; i++) {
        int h = (int)((double)d->days[i].tokens / (double)peak * plot_h);
        if (h < 2 && d->days[i].tokens > 0) h = 2;
        canvas_fill_rect(c, i * slot + 2, bottom - h, bar_w, h, pal_accent(i));
        /* Labels are 5 chars ("09-01"); print every other one when tight. */
        if (slot >= 6 * FONT_W * c->scale || (i % 2) == 0)
            canvas_puts(c, (i * slot) / (FONT_W * c->scale), c->rows - 1,
                        d->days[i].label, PAL_DIM);
    }

    char value[16];
    human(peak, value, sizeof value);
    canvas_puts(c, c->cols - 8, 1, value, PAL_DIM);
}
```

- [ ] **Step 3: Add the sources to `main/CMakeLists.txt`** — extend the `SRCS` list with `"usagedata.c" "pages.c" "views.c"`.

- [ ] **Step 4: Build both boards**

Run: `. tools/idf-env.sh && idf.py build && idf.py -B build-crowpanel -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.crowpanel" -D SDKCONFIG=sdkconfig.crowpanel build`
Expected: both `Project build complete`, no errors.

- [ ] **Step 5: Commit**

```bash
git add main/views.h main/views.c main/CMakeLists.txt
git commit -m "Draw the stats and daily pages"
```

---

### Task 5: `gt911` touch driver

**Files:**
- Create: `main/gt911.h`, `main/gt911.c`
- Modify: `main/CMakeLists.txt`

**Interfaces:**
- Produces: `esp_err_t gt911_init(void)` and `bool gt911_tapped(void)`.

- [ ] **Step 1: Create `main/gt911.h`**

```c
#ifndef GT911_H
#define GT911_H

#include "esp_err.h"
#include <stdbool.h>

/* Probes both known addresses. Returns ESP_ERR_NOT_FOUND when the controller
   does not answer; the caller carries on without touch. */
esp_err_t gt911_init(void);

/* True once per press, reported on release. Poll this. */
bool gt911_tapped(void);

#endif /* GT911_H */
```

- [ ] **Step 2: Create `main/gt911.c`**

```c
#include "gt911.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

#define PIN_SCL 20
#define PIN_SDA 19

/* Which address answers depends on how RST floats at power-on, and RST is not
   wired on this board, so probe both. */
#define ADDR_A 0x5D
#define ADDR_B 0x14

#define REG_STATUS 0x814E

static const char *TAG = "gt911";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_present;
static bool s_was_down;

static esp_err_t read_status(uint8_t *status)
{
    uint8_t reg[2] = { REG_STATUS >> 8, REG_STATUS & 0xFF };
    return i2c_master_transmit_receive(s_dev, reg, sizeof reg, status, 1,
                                       pdMS_TO_TICKS(20));
}

static esp_err_t clear_status(void)
{
    uint8_t clear[3] = { REG_STATUS >> 8, REG_STATUS & 0xFF, 0x00 };
    return i2c_master_transmit(s_dev, clear, sizeof clear, pdMS_TO_TICKS(20));
}

esp_err_t gt911_init(void)
{
    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &s_bus));

    const uint8_t addrs[2] = { ADDR_A, ADDR_B };
    for (int i = 0; i < 2; i++) {
        if (i2c_master_probe(s_bus, addrs[i], 50) != ESP_OK) continue;
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = 400000,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev, &s_dev));
        s_present = true;
        ESP_LOGI(TAG, "GT911 answered at 0x%02X", addrs[i]);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "no GT911 at 0x%02X or 0x%02X; continuing without touch",
             ADDR_A, ADDR_B);
    return ESP_ERR_NOT_FOUND;
}

bool gt911_tapped(void)
{
    if (!s_present) return false;

    uint8_t status = 0;
    if (read_status(&status) != ESP_OK) return false;

    bool down = (status & 0x80) && (status & 0x0F) > 0;
    if (status & 0x80) clear_status();   /* the controller latches until cleared */

    /* Fire on release, so a resting finger does not cycle pages. */
    bool tapped = s_was_down && !down;
    s_was_down = down;
    return tapped;
}
```

- [ ] **Step 3: Add to `main/CMakeLists.txt`** — add `"gt911.c"` to `SRCS` and `esp_driver_i2c` to `REQUIRES`.

- [ ] **Step 4: Build the big board**

Run: `. tools/idf-env.sh && idf.py -B build-crowpanel -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.crowpanel" -D SDKCONFIG=sdkconfig.crowpanel build`
Expected: `Project build complete`.

- [ ] **Step 5: Commit**

```bash
git add main/gt911.h main/gt911.c main/CMakeLists.txt
git commit -m "Add the GT911 touch driver"
```

---

### Task 6: Wire it together in `main`

**Files:**
- Modify: `main/main.c`, `main/display.h`, `main/display_rgb.c`, `main/display_st7789.c`

**Interfaces:**
- Consumes: everything above.
- Produces: `canvas_t *display_canvas(void)` and `void display_blit(void)`, so `main` can drive views without each backend duplicating them.

- [ ] **Step 1: Expose the canvas from the display backends** — add to `main/display.h`:

```c
#include "canvas.h"

/* The backend's canvas, so callers can draw views into it. */
canvas_t *display_canvas(void);

/* Pushes the canvas to the panel. */
void display_blit(void);
```

In **both** `main/display_rgb.c` and `main/display_st7789.c`, rename the private
`static void blit(void)` to `void display_blit(void)`, update the two callers in
that file, and add:

```c
canvas_t *display_canvas(void) { return &s_canvas; }
```

- [ ] **Step 2: Rewrite `main/main.c`**

```c
#include "display.h"
#include "ble_uart.h"
#include "gt911.h"
#include "pages.h"
#include "timecalc.h"
#include "usagedata.h"
#include "views.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <stdbool.h>
#include <string.h>

static const char *TAG = "main";

#define TICK_MS 50            /* also the touch poll interval */
#define MESSAGE_MAX 512

static uint32_t s_base_secs;
static int64_t  s_base_us;
static bool     s_synced;

static usagedata_t s_data;
static char s_message[MESSAGE_MAX + 1];
static pages_t s_pages;

/* Forces a redraw when the page or the displayed minute changes. */
static page_t s_drawn_page = PAGE_COUNT;
static int s_drawn_second = -1;

static void on_time(uint32_t secs)
{
    s_base_secs = secs;
    s_base_us = esp_timer_get_time();
    s_synced = true;
    s_drawn_second = -1;
}

static void on_message(const char *text, size_t len)
{
    int64_t now = esp_timer_get_time();

    ud_kind_t kind = usagedata_parse(&s_data, text);
    if (kind == UD_STATS) {
        pages_show(&s_pages, PAGE_STATS, now);
    } else if (kind == UD_DAILY) {
        pages_show(&s_pages, PAGE_DAILY, now);
    } else if (len == 0) {
        s_message[0] = '\0';
        pages_show(&s_pages, PAGE_CLOCK, now);
    } else {
        size_t n = len < MESSAGE_MAX ? len : MESSAGE_MAX;
        memcpy(s_message, text, n);
        s_message[n] = '\0';
        pages_show(&s_pages, PAGE_MESSAGE, now);
    }
    s_drawn_page = PAGE_COUNT;    /* force a redraw */
}

static void draw_clock(canvas_t *c, int64_t now)
{
    if (!s_synced) {
        if (s_drawn_second != -2) {
            s_drawn_second = -2;
            canvas_big(c, "--:--:--");
            display_blit();
        }
        return;
    }
    uint32_t secs = timecalc_advance(s_base_secs, (uint64_t)(now - s_base_us));
    if ((int)secs == s_drawn_second) return;
    char buf[9];
    timecalc_format_hms(secs, buf);
    canvas_big(c, buf);
    display_blit();
    s_drawn_second = (int)secs;
}

void app_main(void)
{
    if (display_init() != ESP_OK) {
        ESP_LOGE(TAG, "display init failed; halting");
        return;
    }

    unsigned available = PAGE_BIT(PAGE_CLOCK) | PAGE_BIT(PAGE_MESSAGE);
#ifdef CONFIG_SCREEN_BOARD_CROWPANEL_7
    available |= PAGE_BIT(PAGE_STATS) | PAGE_BIT(PAGE_DAILY);
    bool touch = gt911_init() == ESP_OK;
#else
    bool touch = false;
#endif
    pages_init(&s_pages, available);

    if (ble_uart_start(on_message, on_time) != ESP_OK) {
        ESP_LOGE(TAG, "ble start failed");
        canvas_text(display_canvas(), "BLE FAILED");
        display_blit();
        return;
    }

    canvas_t *c = display_canvas();
    int64_t last_beat = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        int64_t now = esp_timer_get_time();

        if (touch && gt911_tapped()) {
            page_t p = pages_advance(&s_pages, now);
            ESP_LOGI(TAG, "tap -> page %d", (int)p);
            s_drawn_page = PAGE_COUNT;
        }

        if (pages_idle_expired(&s_pages, now)) {
            pages_show(&s_pages, PAGE_CLOCK, now);
            s_pages.last_activity_us = 0;   /* the clock does not time out */
            s_drawn_page = PAGE_COUNT;
        }

        if (s_pages.current != s_drawn_page) {
            s_drawn_page = s_pages.current;
            s_drawn_second = -1;
            switch (s_pages.current) {
            case PAGE_STATS:   views_stats(c, &s_data); display_blit(); break;
            case PAGE_DAILY:   views_daily(c, &s_data); display_blit(); break;
            case PAGE_MESSAGE: canvas_text(c, s_message); display_blit(); break;
            default: break;
            }
        }
        if (s_pages.current == PAGE_CLOCK) draw_clock(c, now);

        if (now - last_beat > 30 * 1000000LL) {
            last_beat = now;
            ESP_LOGI(TAG, "alive, page %d, clock %s",
                     (int)s_pages.current, s_synced ? "synced" : "unset");
        }
    }
}
```

- [ ] **Step 3: Build both boards**

Run: `. tools/idf-env.sh && idf.py build && idf.py -B build-crowpanel -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.crowpanel" -D SDKCONFIG=sdkconfig.crowpanel build`
Expected: both complete with no errors.

- [ ] **Step 4: Flash the big board and confirm it still runs**

Run the flash command from Global Constraints, then `tell --device big "wired up"`.
Expected: the text appears; `idf.py -p /dev/cu.usbserial-* monitor` shows either
`GT911 answered at 0x..` or the "continuing without touch" warning.

- [ ] **Step 5: Commit**

```bash
git add main/main.c main/display.h main/display_rgb.c main/display_st7789.c
git commit -m "Wire touch, pages and views into main"
```

---

### Task 7: Send the data from the Mac

**Files:**
- Modify: `tools/claude-stats.py`
- Create: `tools/push-stats.sh`

**Interfaces:**
- Produces: `claude-stats.py --format data --section stats|daily`, emitting the marker lines Task 2 parses.

- [ ] **Step 1: Add the format option to `tools/claude-stats.py`** — add these functions above `main()`:

```python
def render_data(stats, section):
    """Marker-prefixed lines for the firmware to parse."""
    lines = []
    if section == "stats":
        lines.append("!stats")
        ranked = sorted(stats["models"].items(),
                        key=lambda kv: -(kv[1]["cread"] + kv[1]["out"]))
        for name, m in ranked[:8]:
            lines.append("m %s %d %d" % (name[:15], m["out"], m["cread"]))
    else:
        lines.append("!daily")
        for day, total in sorted(stats["daily"].items())[-14:]:
            lines.append("d %s %d" % (day[5:], total))
    return lines
```

and extend the argument parser and dispatch in `main()`:

```python
    ap.add_argument("--format", choices=("ascii", "data"), default="ascii")
    ap.add_argument("--section", choices=("stats", "daily"), default="stats")
    a = ap.parse_args()

    if a.self_test:
        sys.exit(self_test())

    stats = collect()
    if a.format == "data":
        print("\n".join(render_data(stats, a.section)))
    else:
        print("\n".join(render(stats, a.cols, a.rows)))
```

- [ ] **Step 2: Verify the output shape**

Run: `tools/claude-stats.py --format data --section stats | head -3`
Expected: a `!stats` line followed by `m <name> <digits> <digits>` rows.

Run: `tools/claude-stats.py --format data --section daily | head -3`
Expected: a `!daily` line followed by `d MM-DD <digits>` rows.

Run: `tools/claude-stats.py | head -1`
Expected: unchanged ASCII frame — the default must not have moved.

- [ ] **Step 3: Create `tools/push-stats.sh`**

```sh
#!/bin/sh
# Push both usage pages to a board. Two messages, because each marker
# replaces exactly one page.
set -e
cd "$(dirname "$0")/.."
DEVICE="${1:-big}"

./tools/claude-stats.py --format data --section stats | ./mac/tell --device "$DEVICE"
sleep 1
./tools/claude-stats.py --format data --section daily | ./mac/tell --device "$DEVICE"
echo "pushed stats and daily to $DEVICE"
```

Then `chmod +x tools/push-stats.sh`.

- [ ] **Step 4: Push to the board**

Run: `tools/push-stats.sh big`
Expected: the board switches to the stats page, then the daily page.

- [ ] **Step 5: Commit**

```bash
git add tools/claude-stats.py tools/push-stats.sh
git commit -m "Send usage data to the board"
```

---

### Task 8: Verify colour and touch on hardware

**Files:**
- Modify (only if the checks below fail): `main/display_rgb.c`, `main/display_st7789.c`

- [ ] **Step 1: Check the colour channels on the big board**

Run: `tools/push-stats.sh big`
Expected: the title bar is **blue** (`PAL_TITLE_BG` = `0x001F`) and the first
model's bar is **cyan** (`PAL_A0` = `0x07FF`).

If the title bar is red, the red and blue channel groups are swapped: reverse
the two five-pin groups in `RGB_DATA_PINS` in `main/display_rgb.c`, so
`{15,7,6,5,4, 9,46,3,8,16,1, 14,21,47,48,45}` becomes
`{14,21,47,48,45, 9,46,3,8,16,1, 15,7,6,5,4}`. Rebuild, reflash, recheck.

- [ ] **Step 2: Check the colour channels on the small board**

Run: `tell --device small "colour"` then tap through to a page with colour, or
temporarily colour the message text by passing `PAL_A0` in `canvas_text`.
Expected: colours match the big board.

If they do not, RGB565 over SPI needs byte-swapped pixels: set
`.flags.lsb_first = 1` in `esp_lcd_panel_io_spi_config_t` in
`main/display_st7789.c`. Rebuild, reflash, recheck.

- [ ] **Step 3: Check touch**

Tap the big screen four times, watching `idf.py -p /dev/cu.usbserial-* monitor`.
Expected: one `tap -> page N` per tap, cycling 1, 2, 3, 0, and the display
changing to match. A finger held down must produce exactly one tap.

- [ ] **Step 4: Check the idle fallback**

Tap to the stats page and leave it. Expected: the clock returns after 5 minutes.

- [ ] **Step 5: Measure the redraw cost**

Add a temporary `int64_t t0 = esp_timer_get_time();` around the page redraw in
`main.c` and log the delta. Expected: under 50 ms, so a tap feels immediate.
If it is slower, note it; the fix is redrawing only the changed region, which is
out of scope for this plan. Remove the timing code before committing.

- [ ] **Step 6: Commit any corrections**

```bash
git add -A
git commit -m "Correct colour channel order after checking on hardware"
```

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
|---|---|
| `canvas` colour and rectangles | 1 |
| Palette defined once | 1 |
| Protocol and `usagedata` | 2 |
| Page model, idle fallback, reduced set on the Feather | 3 |
| Stats and daily pages, "no data" state | 4 |
| GT911, polling, both addresses, tap on release | 5 |
| Wiring, board-specific page set | 6 |
| `--format` on the sender, `push-stats.sh` | 7 |
| Colour verification on both boards, touch, redraw cost | 8 |

Error-handling table: malformed line, missing fields, unknown marker, model cap
and name truncation are covered by Task 2's tests; "no data" by Task 4;
GT911 absent by Task 5; touch held down by Tasks 5 and 8.

**Type consistency:** `usagedata_parse` returns `ud_kind_t` in Tasks 2, 6.
`pages_advance`/`pages_show`/`pages_idle_expired` signatures identical in Tasks
3 and 6. `canvas_fill_rect` and `canvas_puts` identical in Tasks 1 and 4.
`display_canvas`/`display_blit` declared in Task 6 Step 1 and used in Step 2.

**Placeholders:** none.

**Known gap, deliberate:** `views.c` has no host test. Its output is a picture,
and asserting pixel positions would lock in layout decisions that will change
the first time they are seen on glass. The bounds-clamping that would actually
corrupt memory is tested in `canvas` instead, which is where the risk lives.
