#include "airlog.h"

#include <string.h>

void airlog_init(airlog_t *l)
{
    memset(l, 0, sizeof *l);
}

static void push(air_point_t *ring, uint16_t len, uint16_t *n, uint16_t *at, const air_point_t *p)
{
    ring[*at] = *p;
    *at = (uint16_t)((*at + 1) % len);
    if (*n < len) (*n)++;
}

static void roll(int32_t *sum, uint16_t *cnt, const air_point_t *p, air_point_t *avg)
{
    for (int k = 0; k < AIR_N; k++) {
        avg->v[k] = cnt[k] ? (int16_t)(sum[k] / cnt[k]) : AIRLOG_NONE;
        sum[k] = 0;
        cnt[k] = 0;
    }
    (void)p;
}

bool airlog_minute(airlog_t *l, const air_point_t *p)
{
    push(l->hour, AIRLOG_HOUR, &l->hour_n, &l->hour_at, p);
    for (int k = 0; k < AIR_N; k++)
        if (p->v[k] != AIRLOG_NONE) { l->qsum[k] += p->v[k]; l->qcnt[k]++; }
    if (++l->qmins < 15) return false;
    l->qmins = 0;
    air_point_t q;
    roll(l->qsum, l->qcnt, p, &q);
    push(l->day, AIRLOG_DAY, &l->day_n, &l->day_at, &q);
    for (int k = 0; k < AIR_N; k++)
        if (q.v[k] != AIRLOG_NONE) { l->wsum[k] += q.v[k]; l->wcnt[k]++; }
    if (++l->wquarters >= 8) {
        l->wquarters = 0;
        air_point_t w;
        roll(l->wsum, l->wcnt, &q, &w);
        push(l->week, AIRLOG_WEEK, &l->week_n, &l->week_at, &w);
    }
    return true;
}

int airlog_series(const airlog_t *l, int reading, air_range_t r, int16_t *out)
{
    const air_point_t *ring;
    uint16_t len, n, at;
    switch (r) {
    case AIR_RANGE_HOUR: ring = l->hour; len = AIRLOG_HOUR; n = l->hour_n; at = l->hour_at; break;
    case AIR_RANGE_DAY:  ring = l->day;  len = AIRLOG_DAY;  n = l->day_n;  at = l->day_at;  break;
    default:             ring = l->week; len = AIRLOG_WEEK; n = l->week_n; at = l->week_at; break;
    }
    for (int i = 0; i < n; i++) out[i] = ring[(at + len - n + i) % len].v[reading];
    return n;
}

#define AIRLOG_MAGIC 0x41495231u   /* "AIR1" */

size_t airlog_save(const airlog_t *l, uint8_t *buf, size_t cap)
{
    /* Everything but the hour: a minute-by-minute hour is gone in an hour
       anyway, and it is most of the size. */
    size_t need = 4 + offsetof(airlog_t, qmins) + sizeof l->qmins + sizeof l->wquarters - offsetof(airlog_t, day);
    if (cap < need) return 0;
    uint32_t m = AIRLOG_MAGIC;
    memcpy(buf, &m, 4);
    memcpy(buf + 4, (const uint8_t *)l + offsetof(airlog_t, day), need - 4);
    return need;
}

bool airlog_load(airlog_t *l, const uint8_t *buf, size_t len)
{
    size_t need = 4 + offsetof(airlog_t, qmins) + sizeof l->qmins + sizeof l->wquarters - offsetof(airlog_t, day);
    uint32_t m;
    if (len != need) return false;
    memcpy(&m, buf, 4);
    if (m != AIRLOG_MAGIC) return false;
    airlog_init(l);
    memcpy((uint8_t *)l + offsetof(airlog_t, day), buf + 4, need - 4);
    l->hour_n = l->hour_at = 0;            /* the hour was not saved */
    if (l->day_n > AIRLOG_DAY || l->week_n > AIRLOG_WEEK || l->day_at >= AIRLOG_DAY || l->week_at >= AIRLOG_WEEK) {
        airlog_init(l);
        return false;
    }
    return true;
}
