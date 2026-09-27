#ifndef AIRUI_H
#define AIRUI_H

/*
 * speaker's Air page: the ENS160 + AHT21 module on her I2C bus, read as envo
 * reads the same pair, and set as envo sets it -- one big reading, a word
 * for it in the state's colour (GOOD blue, FAIR amber, POOR black on red),
 * limits and hysteresis from envstate.c -- but upright, 240x320:
 *
 *   - VOC, ppb: the big number and its word, and a trend arrow when moving;
 *   - eCO2 est, ppm: a smaller number and its word (never GOOD: below its
 *     first limit it says FROM VOCs, as envo does -- eCO2 is worked out from
 *     the VOCs, not measured);
 *   - the room: temperature and humidity, as the AHT21 reads them;
 *   - while the gas sensor warms up, WARMING UP and how long so far in the
 *     readings' place, never a number the chip does not vouch for.
 *
 * Pure: drawn from a struct, tested and rendered on the host.
 */
#include "canvas.h"
#include "envstate.h"

#include <stdbool.h>
#include <stdint.h>

#define AIRUI_WIDTH   240
#define AIRUI_HEIGHT  320

typedef struct {
    bool have_sensor;       /* an ENS160 answered at boot */
    bool warming;           /* no valid gas reading yet (ENS160 validity 1 or 2) */
    bool gas_error;         /* validity 3 */
    int  warm_minutes;      /* since the sensor started, for WARMING UP */
    bool have_voc, have_eco2;
    int32_t voc_ppb, eco2_ppm;
    envs_state_t voc_state, eco2_state;
    envs_trend_t voc_trend;
    bool have_temp, have_rh;
    float temp_c, rh;
} airui_t;

void airui_draw(canvas_t *c, const airui_t *s);

/* The last 24 hours as 5-minute means, oldest first, the last slot the one
   running now. NaN is no reading (a gap). `now_slot` is the time of day of
   the last slot, 0..287, for the hour marks. */
#define AIRUI_SLOTS  288
typedef struct {
    float voc[AIRUI_SLOTS];     /* ppb */
    float eco2[AIRUI_SLOTS];    /* ppm */
    int   now_slot;
    bool  have_sensor;
} airui_day_t;

/* AIR 24H: VOC and eCO2 est on fixed zoned scales, each trace coloured by
   the state it is in. */
void airui_draw_day(canvas_t *c, const airui_day_t *d);

/* The week: 7 days of 24 hours, today last. Each hour the worst state that
   lasted 15 minutes of it (3 of its 5-minute means), of VOC and eCO2. */
typedef enum { AIRUI_CELL_NONE = 0, AIRUI_CELL_OK, AIRUI_CELL_FAIR, AIRUI_CELL_POOR, AIRUI_CELL_FUTURE } airui_cell_t;
typedef struct {
    uint8_t cell[7][24];        /* airui_cell_t */
    char    day[7][3];          /* "Mo".."Su", today last */
    int     poor_hours, fair_hours;
    bool    have_sensor;
} airui_week_t;

void airui_draw_week(canvas_t *c, const airui_week_t *w);

#endif /* AIRUI_H */
