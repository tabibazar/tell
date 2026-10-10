#ifndef KNOCK_H
#define KNOCK_H

/*
 * A knock on the desk, or a nudge to the panel, from the accelerometer:
 * the change in acceleration between two readings (a jolt) above a
 * threshold, then a quiet spell so one knock is not counted three times.
 * Gravity and the panel's tilt do not matter -- only change does. Pure:
 * host_tests/test_sc_rest.c.
 */
#include <stdbool.h>

#define KNOCK_G        0.08f     /* jolt that counts, in g between readings ~30 ms apart */
#define KNOCK_QUIET_MS 180       /* enough to stop one knock ringing into three at ~30 ms readings */

/* A second knock this soon to this long after the first makes a pair. */
#define KNOCK2_MIN_MS 250
#define KNOCK2_MAX_MS 900

/* first_at sentinel: no pending first knock. Comfortably below any real
   t_ms (which starts near 0), so t_ms == 0 is never mistaken for one. */
#define KNOCK_NONE (-100000L)

typedef struct {
    float ax, ay, az;
    bool primed;
    long quiet_until;
    float last_jolt;             /* for tuning: the latest jolt seen */
    long first_at;                /* a pending first knock's time, or KNOCK_NONE */
    long hush_until;               /* ignore every reading before this time */
} knock_t;

void knock_init(knock_t *k);

/* One accelerometer reading (g) at time t_ms; true on a knock. False while
   t_ms < hush_until, though ax/ay/az are still tracked so the next reading
   after the hush does not see a false jolt. */
bool knock_feed(knock_t *k, float ax, float ay, float az, long t_ms);

/* Ignore everything until until_ms (a finger on the screen jolts the panel
   too), and forget a pending first knock. */
void knock_hush(knock_t *k, long until_ms);

/* Call when knock_feed returned true: true when this is the second knock
   250-900 ms after the first (then the pair is consumed); otherwise it
   becomes the new first knock. */
bool knock_double(knock_t *k, long t_ms);

#endif /* KNOCK_H */
