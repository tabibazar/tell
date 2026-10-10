#ifndef REST_H
#define REST_H

/*
 * How awake stan-claw's screen should be. Untouched for 45 s it starts
 * breathing (a dim orb, the backlight rising and falling); 5 minutes after
 * that the backlight goes off. From 19:00 to 08:00 it skips the breathing
 * and goes dark at 45 s. Pure: host_tests/test_sc_rest.c.
 */
#include <stdbool.h>

#define REST_BREATHE_S   45
#define REST_OFF_S       (REST_BREATHE_S + 5 * 60)
#define REST_NIGHT_FROM  (19 * 60)
#define REST_NIGHT_TO    (8 * 60)
#define REST_BREATH_MS   4000     /* one breath, in and out */

typedef enum { REST_AWAKE, REST_BREATHING, REST_ASLEEP } rest_level_t;

/* idle_s: seconds since the last touch, press, conversation or agent call.
   minute: minute of the local day, or -1 when the clock is not set yet. */
rest_level_t rest_level(int idle_s, int minute);

/* The backlight while breathing, percent, at time t_ms into the rest: a
   smooth rise from 3 % to 14 % and back over REST_BREATH_MS. */
int rest_breath_light(long t_ms);

#endif /* REST_H */
