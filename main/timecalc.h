#ifndef TIMECALC_H
#define TIMECALC_H

#include <stdint.h>

#define SECS_PER_DAY 86400u

/* Advances a seconds-since-midnight base by an elapsed microsecond count,
   wrapping at midnight. Pure: no clock, no hardware. */
uint32_t timecalc_advance(uint32_t base_secs, uint64_t elapsed_us);

/* Splits seconds-since-midnight into hours and minutes. */
void timecalc_split(uint32_t secs, int *h, int *m);

/* Formats seconds-since-midnight as "HH:MM". `out` needs 6 bytes. */
void timecalc_format(uint32_t secs, char out[6]);

/* Formats seconds-since-midnight as "HH:MM:SS". `out` needs 9 bytes. */
void timecalc_format_hms(uint32_t secs, char out[9]);

/* Days since 1970-01-01 to a civil date. The Mac sends days as a count so
   the board never handles time zones; this turns the count back into a
   calendar for month labels and day names. */
void timecalc_civil(int32_t days, int *y, int *m, int *d);

/*
 * The time now, from a base reading and two microsecond timestamps.
 *
 * The clamp is the whole point. The board re-bases its clock from the RTC
 * once an hour, in the middle of a loop iteration that captured `now` at the
 * top -- so `now` can be *older* than the base by a few milliseconds. Done as
 * unsigned subtraction that becomes eighteen quintillion microseconds, and
 * advancing a clock by six hundred thousand years lands it at an arbitrary
 * time of day.
 *
 * That is not hypothetical: it put one corrupt reading an hour into wave's
 * environment log, every hour, with a plausible temperature attached to a
 * timestamp sixteen hours out.
 */
uint32_t timecalc_since(uint32_t base_secs, int64_t base_us, int64_t now_us);

/* The inverse: a civil date to days since 1970-01-01. Needed to stamp a log
   entry with a real date rather than with time since power-on. */
int32_t timecalc_days(int y, int m, int d);

/* Weekday of a day count: 0 Sunday .. 6 Saturday. */
int timecalc_weekday(int32_t days);

/* "Jan".."Dec" for a month 1..12. */
const char *timecalc_month_abbr(int m);

#endif /* TIMECALC_H */
