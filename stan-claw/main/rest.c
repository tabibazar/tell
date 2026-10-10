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

bool rest_office(int wday, int minute)
{
    return wday >= 1 && wday <= 5 && minute >= REST_OFFICE_FROM && minute < REST_OFFICE_TO;
}

long rest_until_office(int wday, int minute, int second)
{
    if (rest_office(wday, minute)) return 0;
    long now = (long)minute * 60 + second;
    for (int d = 0; d < 8; d++) {
        int wd = (wday + d) % 7;
        if (wd < 1 || wd > 5) continue;
        long start = (long)d * 86400 + REST_OFFICE_FROM * 60L;
        if (start > now) return start - now;
    }
    return 86400;    /* not reached */
}
