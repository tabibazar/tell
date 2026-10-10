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
#define KNOCK_QUIET_MS 600

typedef struct {
    float ax, ay, az;
    bool primed;
    long quiet_until;
    float last_jolt;             /* for tuning: the latest jolt seen */
} knock_t;

void knock_init(knock_t *k);

/* One accelerometer reading (g) at time t_ms; true on a knock. */
bool knock_feed(knock_t *k, float ax, float ay, float az, long t_ms);

#endif /* KNOCK_H */
