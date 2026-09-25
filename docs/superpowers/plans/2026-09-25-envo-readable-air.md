# envo Readable Air Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace envo's unreadable auto-scaled env pages with one big reading
per page (VOC home, eCO2 est, TEMP, HUMIDITY), fixed-scale 24 h strips, a
tap-for-detail full chart, a week grid and a clock verdict.

**Architecture:** Two new pure, host-tested C modules. `envstate.c` holds the
limits, state, hysteresis, display value, trend and the 5-minute fold.
`envui.c` draws every new page from plain data structs. `main.c` only feeds
them (30 s readings in, 24 h/7 d slot arrays built from the flash ring) and
owns paging and touch.

**Tech Stack:** ESP-IDF 5.5 C (C11), the repo's `canvas.c`, `vector.c` (AA
drawing) and `vfont.c` (stroke font), and host tests with cc and asserts.

**Spec:** `docs/superpowers/specs/2026-09-25-envo-readable-air-design.md`.
Research, mockups and reference renders: `docs/design/envo-ui/`.

## Global Constraints

- Panel 320x172 landscape, RGB565. Text within x 16..304.
- Colours:
  - GOOD `0x04BF`, FAIR `0xFD40`, POOR block `0xF8C1` with black text.
  - Grey labels `0x8410`; hairlines and floor `0x5ACB`; white `0xFFFF`.
  - Colour means state only, and a word always accompanies it.
- Limits:
  - VOC 220 / 650 ppb (chart top 2200, floor 0).
  - eCO2 800 / 1000 ppm (chart top 1500, floor 400).
  - Hysteresis: worse at once; better only when value < 0.9 x edge.
- **The display value** is the median of the last 4 valid 30 s readings.
  - Truncated for display: VOC in steps of 5 below 100, 10 below 1000, 100
    above; eCO2 in 10; TEMP 0.1 C; RH 1 %.
- **Trend:** mean of the last 10 min against the mean of the 10 min ending
  30 min ago.
  - Moving means: VOC at least max(15 ppb, 25 %); eCO2 at least 50 ppm; TEMP
    at least 0.5 C; RH at least 3 %.
  - None before 40 min of valid data.
- **eCO2 never shows GOOD,** and can make the verdict worse, never better.
- **Invalid gas readings** (validity != 0) never set a scale, word or colour;
  they are gaps.
- **TEMP and RH are raw AHT21 values:** no offset, no "est", no comfort words.
- **Font limits:** vfont draws A-Z 0-9 space `. , : - / + % \`. Lower case, `~`
  and the degree sign are not in vfont: draw the degree with `vec_ring` and
  lower-case labels with the 12x24 bitmap font.
- **Tests:** host tests `cc -std=c11 -Wall -Wextra -Werror -g -I../main` from
  `host_tests/`, with rules in `host_tests/Makefile` and binaries added to
  `.gitignore`.
- **Other boards** (lilly, wave, watch, envio configs) must still build and
  behave as before; envo-only code sits behind
  `CONFIG_SCREEN_BOARD_TOUCH_LCD_147`.

---

### Task 1: envstate -- limits, state, display value, trend, 5-minute fold

**Files:**
- Create: `main/envstate.h`, `main/envstate.c`
- Create: `host_tests/test_envstate.c`
- Modify: `host_tests/Makefile` (a `test_envstate` rule, added to `all` and `clean`), `.gitignore`

**Interfaces:**
- Consumes: `env_sample_t` and the `ENV_HAVE_*` / `ENV_GAS_FLAGS` definitions from `main/envstore.h`.
- Produces: exactly this header, which Tasks 2 and 3 rely on:

```c
#ifndef ENVSTATE_H
#define ENVSTATE_H
#include "envstore.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum { ENVS_VOC = 0, ENVS_ECO2, ENVS_TEMP, ENVS_RH, ENVS_N } envs_series_t;
typedef enum { ENVS_OK = 0, ENVS_FAIR, ENVS_POOR, ENVS_WAIT } envs_state_t;   /* WAIT: gas warming up or invalid */
typedef enum { ENVS_UNKNOWN = 0, ENVS_STEADY, ENVS_RISING, ENVS_FALLING } envs_trend_t;

/* Values are integers in the series' own unit: VOC ppb, eCO2 ppm, TEMP 0.01 C, RH 0.01 %. */
typedef struct { int32_t fair, poor, top, floor; } envs_limits_t;
const envs_limits_t *envs_limits(envs_series_t s);        /* NULL for TEMP/RH */
envs_state_t envs_classify(envs_series_t s, int32_t v, envs_state_t prev);
int32_t envs_quantise(envs_series_t s, int32_t v);         /* truncation steps above */

#define ENVS_RING 96                                        /* 48 min of 30 s readings */
typedef struct {
    int64_t t_us;
    int16_t temp_c100; uint16_t rh_c100;
    uint16_t tvoc, eco2; uint8_t aqi, validity;             /* validity: ENS160 0..3 */
    uint8_t have;                                           /* ENV_HAVE_TEMP|RH|GAS */
} envs_reading_t;

typedef struct {
    envs_reading_t r[ENVS_RING];
    int n, head;
    int64_t gas_start_us;                                   /* first reading after power-on, for "N MIN SO FAR" */
    envs_state_t state[ENVS_N];                             /* with hysteresis */
} envs_t;

void envs_init(envs_t *e);
void envs_push(envs_t *e, const envs_reading_t *r);
bool envs_value(const envs_t *e, envs_series_t s, int32_t *out);   /* median of last 4 valid */
envs_trend_t envs_trend(const envs_t *e, envs_series_t s, int64_t now_us);
envs_state_t envs_update(envs_t *e, envs_series_t s);              /* recompute state[s]; WAIT for gas with no valid reading */
bool envs_gas_warming(const envs_t *e, int *minutes_so_far, bool *error, int64_t now_us);

typedef struct { envs_series_t worst; envs_state_t state; } envs_verdict_t;
envs_verdict_t envs_verdict(const envs_t *e);   /* worse of VOC and eCO2 states; VOC on a tie; WAIT if gas warming */

#define ENV_MEANPEAK 0x40   /* flash record: tvoc/eco2 are 5-minute means, hpa_x10 holds the TVOC max */
/* Folds the valid readings with t_us in [from_us, to_us) into one record:
   means of temp/rh/tvoc/eco2, max tvoc in hpa_x10, ENV_MEANPEAK set, ENV_HAVE_HPA clear.
   With no valid gas reading, the latest validity is kept and ENV_HAVE_GAS clear.
   Returns false when nothing at all was measured. `minute` is set by the caller. */
bool envs_fold5(const envs_t *e, int64_t from_us, int64_t to_us, env_sample_t *out);
#endif
```

- [ ] **Step 1: Write failing tests** in `host_tests/test_envstate.c`, asserting at least:
  - `envs_limits(ENVS_VOC)->fair == 220 && ->poor == 650 && ->top == 2200`, and eCO2 `800/1000/1500/400`.
  - Classify VOC 219 from OK is OK; 220 is FAIR; 649 is FAIR; 650 is POOR. Hysteresis from FAIR: 199 stays FAIR, 197 is OK (below 198). From POOR: 590 stays POOR, 584 is FAIR.
  - Quantise VOC: 47 to 45, 218 to 210, 1366 to 1300; eCO2 447 to 440; TEMP 2687 to 2680; RH 3765 to 3700.
  - Median of the last 4 valid readings: pushing tvoc 10, 90, 20, 30, 1000 (the 1000 with validity 2) gives 25 (the median of 10, 90, 20, 30). With 1 valid reading, that reading.
  - Trend: 80 readings at 30 s steps, flat at 50 ppb, then 20 at 120, is RISING. A flat line is STEADY. Under 40 min of data is UNKNOWN. A change of 12 ppb from a base of 40 is STEADY (below max(15, 25 %)).
  - Verdict: VOC OK and eCO2 FAIR gives worst ECO2, FAIR. VOC POOR and eCO2 POOR gives VOC, POOR. eCO2 alone never produces OK over a worse VOC.
  - Fold: 10 readings (two invalid) give mean tvoc over the 8 valid, hpa_x10 = max tvoc, `flags & ENV_MEANPEAK`, `!(flags & ENV_HAVE_HPA)`. All invalid gives ENV_HAVE_GAS clear and the validity carried.
  - Warming: validity 2 readings give `envs_gas_warming` true with minutes = (now - gas_start)/60 s.
- [ ] **Step 2:** `cd host_tests && make test_envstate` fails (no source).
- [ ] **Step 3:** Implement `main/envstate.c` (pure C, no ESP-IDF, no malloc), commenting the why in the codebase's prose style: the datasheet tables, and why truncation rather than rounding.
- [ ] **Step 4:** `make test_envstate && ./test_envstate` passes; `make all` still passes.
- [ ] **Step 5:** Commit `envstate: limits, state, display value, trend, 5-minute fold (host-tested)`.

### Task 2: envui -- draws the new pages from plain data

**Files:**
- Create: `main/envui.h`, `main/envui.c`
- Create: `host_tests/test_envui.c` (writes BMPs to `host_tests/renders/envui/`, which is git-ignored)
- Modify: `host_tests/Makefile`, `.gitignore`

**Interfaces:**
- Consumes: `envstate.h` (Task 1: `envs_series_t`, `envs_state_t`, `envs_trend_t`, `envs_verdict_t`, `envs_limits`), `canvas.h`, `vector.h`, `vfont.h`.
- Produces:

```c
#ifndef ENVUI_H
#define ENVUI_H
#include "canvas.h"
#include "envstate.h"

#define ENVUI_SLOTS 288          /* 24 h of 5-minute means; [ENVUI_SLOTS-1] is the latest */

typedef struct {
    envs_series_t series;
    int32_t slot[ENVUI_SLOTS];   /* series unit, as envstate */
    bool    valid[ENVUI_SLOTS];
    int32_t peak_value;          /* VOC/eCO2 only: the stored 5-minute max; */
    int     peak_slot;           /* -1 when none */
    int     midnight_slot;       /* -1 when midnight is not in the window */
    const char *midnight_day;    /* "FRI" */
    int     last_slot_hour;      /* local hour of the latest slot, for tick labels */
    bool    have_now;            /* display value available */
    int32_t now_value;           /* the display value (unquantised) */
    envs_state_t state;
    envs_trend_t trend;
    bool    warming, gas_error;  /* gas pages: show WARMING UP / GAS ERROR */
    int     warm_minutes;
} envui_series_t;

/* A reading page (style A): label, unit, big number, word, trend, 24 h strip, page dots. */
void envui_reading(canvas_t *c, const envui_series_t *s, int page, int pages);
/* The full 24 h chart shown by a tap. */
void envui_detail(canvas_t *c, const envui_series_t *s);

#define ENVUI_CELL_NONE   0      /* no data */
#define ENVUI_CELL_OK     1
#define ENVUI_CELL_FAIR   2
#define ENVUI_CELL_POOR   3
#define ENVUI_CELL_FUTURE 4
typedef struct {
    uint8_t cell[7][24];         /* row 6 is today */
    char    day[7][3];           /* "MO".."SU" */
    int     poor_hours, fair_hours;
} envui_week_t;
void envui_week(canvas_t *c, const envui_week_t *w, int page, int pages);

/* The clock page's verdict line ("AIR GOOD", "VOC FAIR", "ECO2 POOR", "WARMING UP"),
   left-aligned at (x, y top of capitals), capital height `size` px. */
void envui_verdict(canvas_t *c, float x, float y, float size, envs_verdict_t v);
#endif
```

- [ ] **Step 1: Write failing tests.** A canary-guarded 320x172 buffer. Draw every function with:
  - VOC in GOOD (45), FAIR (260, rising), POOR (1366, rising), warming (12 min), and gas error;
  - eCO2 at 440 and 870; TEMP 26.9; RH 37;
  - a series of all gaps, one with a single valid slot, and one spanning a midnight;
  - the week with mixed cells and all good;
  - the verdict in each state.

  Assert no canary is touched. Assert determinism (the same input gives byte-identical output). Assert the reading-page number's ink height is at least 56 px at 1x.
- [ ] **Step 2:** It fails to build.
- [ ] **Step 3: Implement `main/envui.c`**, following the spec's layouts, `docs/design/envo-ui/mock_rev1.c` (`page_single`) and `mock_rev2.c` (detail chart, week). Gas charts use the three fixed linear zones:
  - GOOD: 0..fair over 42 px;
  - FAIR: fair..poor over 32 px;
  - POOR: poor..top over 24 px, pinned at top.

  TEMP uses a fixed 16..30 C and RH a fixed 20..80 %.
- [ ] **Step 4:** Tests pass. Render BMPs, convert them with `sips -s format png`, and LOOK at them next to `docs/design/envo-ui/ref/*.png`. Iterate until they match the spec.
- [ ] **Step 5:** Commit `envui: style-A reading pages, detail chart, week grid, verdict (host-tested, rendered)`.

### Task 3: main.c integration for envo

**Files:**
- Modify: `main/main.c` (env sampling, env pages, paging and touch for envo, clock verdict)
- Modify: `main/envstore.c/.h` (the CSV `aqi` column, keeping the `envcsv_line` tests green)
- Modify: `host_tests/test_envstore.c`
- Modify: `sdkconfig.defaults.envo` (`CONFIG_SCREEN_BRIGHTNESS=40`)

**Interfaces:**
- Consumes: everything in Tasks 1-2. The existing `envstore_walk`, `envstore_add`, `env_read_averaged`, `env_sdlog`, `env_sample`, `s_pages`, `pages_*`, `touch_tapped/touch_swipe`, `s_wake_grace_us`, and `display_sleep`.

**Steps:**
- [ ] **1. 30 s feed.**
  - Every 30 s sample (today `env_sdlog`'s cadence; make it run with or without an SD card), build an `envs_reading_t` from the same averaged reading and `envs_push` it into a static `envs_t`.
  - Then call `envs_update` for each series.
- [ ] **2. 5-minute flash record.**
  - `env_sample` stores `envs_fold5(window)` with `minute` set, instead of a single reading.
  - Keep `ENV_SYNTHETIC` handling as is.
- [ ] **3. Slot caches.**
  - Keep static `envui_series_t` for VOC, eCO2, TEMP and RH, and a static `envui_week_t`.
  - Build them once at boot by `envstore_walk` over the last 7 days, bucketing by `minute` into 5-minute slots (the 24 h window) and hour cells (the week).
  - Update them incrementally on each 5-minute add, with no flash walk per frame.
  - VOC `peak_value` comes from the records' `hpa_x10` when `ENV_MEANPEAK` is set, and falls back to `tvoc_ppb` for older records.
- [ ] **4. Pages on envo.**
  - The available mask is `PAGE_ROOM_VOC`, `PAGE_ROOM_CO2`, `PAGE_ROOM_TEMP`, `PAGE_ROOM_RH`, `PAGE_WEEK` and `PAGE_CLOCK`, in that page order.
  - `home_page()` returns `PAGE_ROOM_VOC` on envo; waking returns home.
  - The draw paths call `envui_reading` / `envui_detail` / `envui_week`.
  - Redraw when a 30 s sample lands or the minute changes, not every tick.
  - No grow animation on these pages.
- [ ] **5. Touch on envo.**
  - A swipe pages.
  - A tap toggles the detail view on the four reading pages and does nothing on WEEK or CLOCK.
  - The wake tap is still swallowed. Detail closes when the page changes or the screen sleeps.
- [ ] **6. Clock verdict.**
  - On envo the clock's sensor line becomes `envui_verdict(...)` from `envs_verdict`.
- [ ] **7. Removals on envo.**
  - Remove TREND, the pressure page and week, the week rotation, and TEMPS (keep it behind a debug define).
  - Replace the old `voc_thresh` / `co2_thresh` uses with `envs_limits`.
- [ ] **8. CSV.**
  - `envcsv_line` gains a trailing `aqi` column, and `ENVCSV_HEADER` updates.
  - Existing `_2.csv` handling keeps a file's header consistent.
  - Update the `test_envstore` expectations.
- [ ] **9. Brightness.** Set `sdkconfig.defaults.envo` to 40. Also set `sdkconfig.envo` (git-ignored) so the next build uses it.
- [ ] **10. Tests and commit.**
  - Host tests (`make all`) must pass.
  - Commit `envo: readable air -- one big reading per page, fixed-scale history, tap for detail`.

### Task 4: Review, design critique, fix

- [ ] **Lens A, correctness and data integrity:**
  - hysteresis and median edge cases;
  - fold means against invalid data;
  - cache bucketing across midnight, DST and clock steps;
  - no flash walk per frame;
  - old records without `ENV_MEANPEAK`;
  - other boards unaffected.
- [ ] **Lens B, spec fidelity from the renders:**
  - every rule in the spec's Global Constraints;
  - sizes and positions;
  - colour only with words;
  - no invalid data drawn;
  - text within 16..304.
- [ ] **Design critic:** looks at the renders and names concrete fixes; a fixer applies the confirmed ones and re-renders.

### Task 5 (orchestrator, not an agent): build, flash, verify

- [ ] Build envo, lilly, wave and watch.
- [ ] Flash envo with the MAC guard, watch its log for display value, state and trend, and publish the renders to the design page.
- [ ] Reza checks legibility across the room, tap to detail, swipe paging and wake to VOC.
