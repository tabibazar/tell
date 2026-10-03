#ifndef SCHED_H
#define SCHED_H

/*
 * When spy works, as pure functions of local time so the host tests can walk
 * whole weeks through them. Weekdays only: a frame every minute from 07:00 to
 * 16:59, and at 17:00 the whole day's frames go to Telegram as one clip
 * (Reza, 2026-10-03: once at the end of the day rather than every hour).
 */
#include <stdbool.h>
#include <time.h>

#define SCHED_FIRST_HOUR 7      /* first hour with frames */
#define SCHED_LAST_HOUR  16     /* last hour with frames */

/* A time-lapse frame is due in this minute. */
bool sched_capture(const struct tm *t);

/* The day's clip is due now: 17:00 on a weekday. */
bool sched_day_clip(const struct tm *t);

/* "20261002": the day's folder name. */
void sched_day(const struct tm *t, char out[9]);

#endif /* SCHED_H */
