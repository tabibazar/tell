#ifndef AIRLOG_H
#define AIRLOG_H

/*
 * blinky1's air history: one point a minute for the last hour, a quarter of
 * an hour for the last day, two hours for the last week -- each the average
 * of the finer points under it. Four readings: temperature and humidity (in
 * tenths), eCO2 (ppm) and TVOC (ppb). No clock: "the last hour" is the hour
 * before now, whatever time that is. The day and week survive a power cut
 * through airlog_save/load (blinky1 keeps them in NVS). Pure C, host-tested.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { AIR_TEMP, AIR_RH, AIR_ECO2, AIR_TVOC, AIR_N };
#define AIRLOG_HOUR 60      /* minutes */
#define AIRLOG_DAY  96      /* quarter hours */
#define AIRLOG_WEEK 84      /* two hours */
#define AIRLOG_NONE INT16_MIN

typedef struct {
    int16_t v[AIR_N];
} air_point_t;

typedef struct {
    air_point_t hour[AIRLOG_HOUR], day[AIRLOG_DAY], week[AIRLOG_WEEK];
    uint16_t hour_n, day_n, week_n;          /* how many filled */
    uint16_t hour_at, day_at, week_at;       /* the next slot to write */
    int32_t qsum[AIR_N], wsum[AIR_N];        /* roll-ups in progress */
    uint16_t qcnt[AIR_N], wcnt[AIR_N];
    uint16_t qmins, wquarters;
} airlog_t;

void airlog_init(airlog_t *l);

/* One minute's averages. AIRLOG_NONE for a reading not to be had. True when
   this closed a quarter hour (a moment to save). */
bool airlog_minute(airlog_t *l, const air_point_t *p);

typedef enum { AIR_RANGE_HOUR, AIR_RANGE_DAY, AIR_RANGE_WEEK, AIR_RANGES } air_range_t;

/* The series for one reading over one range, oldest first, into out[]
   (sized for the range's length). Returns how many points. */
int airlog_series(const airlog_t *l, int reading, air_range_t r, int16_t *out);

/* What survives a power cut: the day and the week. */
size_t airlog_save(const airlog_t *l, uint8_t *buf, size_t cap);
bool airlog_load(airlog_t *l, const uint8_t *buf, size_t len);

#endif /* AIRLOG_H */
