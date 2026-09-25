#ifndef ENVSTATE_H
#define ENVSTATE_H

/*
 * envo's air, as numbers a person can act on: one table of limits (the
 * ENS160 datasheet v1.3, Tables 5-6), a state per reading with hysteresis so
 * the word does not flicker at an edge, the value to display (the median of
 * the last four valid 30 s readings), a trend, and the 5-minute fold written
 * to flash. Pure: no clock, no hardware -- the caller brings the readings.
 * See docs/superpowers/specs/2026-09-25-envo-readable-air-design.md.
 */
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
