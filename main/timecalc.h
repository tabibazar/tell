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

/* Weekday of a day count: 0 Sunday .. 6 Saturday. */
int timecalc_weekday(int32_t days);

/* "Jan".."Dec" for a month 1..12. */
const char *timecalc_month_abbr(int m);

#endif /* TIMECALC_H */
