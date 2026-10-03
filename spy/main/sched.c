#include "sched.h"

#include <stdio.h>

static bool weekday(const struct tm *t) { return t->tm_wday >= 1 && t->tm_wday <= 5; }

bool sched_capture(const struct tm *t)
{
    return weekday(t) && t->tm_hour >= SCHED_FIRST_HOUR && t->tm_hour <= SCHED_LAST_HOUR;
}

bool sched_day_clip(const struct tm *t)
{
    return weekday(t) && t->tm_hour == SCHED_LAST_HOUR + 1 && t->tm_min == 0;
}

void sched_day(const struct tm *t, char out[9])
{
    unsigned y = (unsigned)(t->tm_year + 1900) % 10000u, m = (unsigned)(t->tm_mon + 1) % 100u,
             d = (unsigned)t->tm_mday % 100u;
    snprintf(out, 9, "%04u%02u%02u", y, m, d);
}
