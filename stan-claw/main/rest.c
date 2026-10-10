#include "rest.h"

#include <math.h>

static bool night(int minute)
{
    return minute >= 0 && (minute >= REST_NIGHT_FROM || minute < REST_NIGHT_TO);
}

rest_level_t rest_level(int idle_s, int minute)
{
    if (idle_s < REST_BREATHE_S) return REST_AWAKE;
    if (night(minute) || idle_s >= REST_OFF_S) return REST_ASLEEP;
    return REST_BREATHING;
}

int rest_breath_light(long t_ms)
{
    /* A raised cosine: slow at the top and bottom, as a breath is. */
    double ph = (double)(t_ms % REST_BREATH_MS) / REST_BREATH_MS;
    double v = 0.5 - 0.5 * cos(2.0 * 3.14159265358979 * ph);
    return 3 + (int)lround(11.0 * v);
}
