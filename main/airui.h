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

#endif /* AIRUI_H */
