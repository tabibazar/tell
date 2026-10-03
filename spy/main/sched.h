#ifndef SCHED_H
#define SCHED_H

/*
 * When spy works, as pure functions of local time so the host tests can walk
 * whole weeks through them. Weekdays only: a frame every minute from 07:00 to
 * 16:59 (every ten minutes otherwise), and at 07:00 every day the 24 hours
 * before go to Telegram as one clip.
 */
#include <stdbool.h>
#include <time.h>

#define SCHED_FIRST_HOUR 7      /* first hour with frames */
#define SCHED_LAST_HOUR  16     /* last hour with frames */

/* A time-lapse frame is due in this minute. */
bool sched_capture(const struct tm *t);

/* Outside those hours -- evenings, nights, weekends -- a frame every ten
   minutes instead (Reza, 2026-10-03): to the card and tiny1, not Telegram,
   and not in the day clip. */
bool sched_capture_quiet(const struct tm *t);

/* The daily clip is due now: 07:00 every day, weekends too, covering the
   24 hours before (Reza, 2026-10-03, replacing the 17:00 day clip and the
   weekday-morning night clip). `*from` gets 07:00 the day before. */
bool sched_daily_clip(const struct tm *t, time_t *from);

/* "20261002": the day's folder name. */
void sched_day(const struct tm *t, char out[9]);

#endif /* SCHED_H */
