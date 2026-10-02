#ifndef SCHED_H
#define SCHED_H

/*
 * When spy works, as pure functions of local time so the host tests can walk
 * whole weeks through them. Weekdays only: a frame every minute from 07:00 to
 * 16:59, and at the top of each hour from 08:00 to 17:00 the clip of the hour
 * before goes to Telegram.
 */
#include <stdbool.h>
#include <time.h>

#define SCHED_FIRST_HOUR 7      /* first hour with frames */
#define SCHED_LAST_HOUR  16     /* last hour with frames */

/* A time-lapse frame is due in this minute. */
bool sched_capture(const struct tm *t);

/* The hour whose clip is due now (at minute 0 of the next), or -1. */
int sched_clip_hour(const struct tm *t);

/* "20261002": the day's folder name. */
void sched_day(const struct tm *t, char out[9]);

#endif /* SCHED_H */
