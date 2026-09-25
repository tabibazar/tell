#ifndef ENVUI_H
#define ENVUI_H

/*
 * envo's air pages, drawn from plain numbers: one big reading per page with a
 * thin 24 h strip under it, the full 24 h chart a tap opens, the week grid,
 * and the clock's one-line verdict.
 *
 * Every scale here is fixed -- the gas zones from envstate's limits table,
 * 16..30 C, 20..80 % -- because the pages this replaces rescaled until sensor
 * noise filled the chart, and a chart whose axis moves cannot be read at a
 * glance. Colour only ever means a state, and a word always goes with it.
 *
 * Pure: no clock, no hardware, no globals from main.c. The caller builds the
 * structs below from envstate and the flash log; this only draws them, so
 * every page can be rendered and checked on the host.
 * See docs/superpowers/specs/2026-09-25-envo-readable-air-design.md.
 */
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

/* The clock page's verdict line ("AIR GOOD", "VOC FAIR", "ECO2 POOR"; "WARMING UP"
   for a verdict of ENVS_WAIT), left-aligned at (x, y top of capitals), capital
   height `size` px. */
void envui_verdict(canvas_t *c, float x, float y, float size, envs_verdict_t v);
/* Its width in px at that size -- the POOR block included -- for centring it. */
float envui_verdict_width(float size, envs_verdict_t v);

/* In the verdict's place while the gas sensor has none to give: "WARMING UP",
   or "GAS ERROR" when the chip has flagged its data invalid (validity 3),
   grey, placed and sized as the verdict is. envs_verdict says ENVS_WAIT for
   both, so the caller asks envs_gas_warming which it is. */
void envui_verdict_wait(canvas_t *c, float x, float y, float size, bool gas_error);
float envui_verdict_wait_width(float size, bool gas_error);
#endif
