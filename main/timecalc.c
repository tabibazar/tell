#include "timecalc.h"

uint32_t timecalc_advance(uint32_t base_secs, uint64_t elapsed_us)
{
    uint64_t secs = (uint64_t)base_secs + elapsed_us / 1000000ull;
    return (uint32_t)(secs % SECS_PER_DAY);
}

void timecalc_split(uint32_t secs, int *h, int *m)
{
    secs %= SECS_PER_DAY;
    *h = (int)(secs / 3600u);
    *m = (int)((secs % 3600u) / 60u);
}

void timecalc_format(uint32_t secs, char out[6])
{
    int h, m;
    timecalc_split(secs, &h, &m);
    out[0] = (char)('0' + h / 10);
    out[1] = (char)('0' + h % 10);
    out[2] = ':';
    out[3] = (char)('0' + m / 10);
    out[4] = (char)('0' + m % 10);
    out[5] = '\0';
}

void timecalc_format_hms(uint32_t secs, char out[9])
{
    secs %= SECS_PER_DAY;
    timecalc_format(secs, out);
    out[5] = ':';
    out[6] = (char)('0' + (secs % 60u) / 10u);
    out[7] = (char)('0' + (secs % 60u) % 10u);
    out[8] = '\0';
}

/* Days since 1970-01-01 to a civil date. Howard Hinnant's algorithm: pure
   integer arithmetic, valid for any date the board will ever be shown. */
void timecalc_civil(int32_t days, int *y, int *m, int *d)
{
    int64_t z = (int64_t)days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;                                  /* [0, 146096] */
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* [0, 399] */
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);           /* [0, 365] */
    int64_t mp = (5 * doy + 2) / 153;                                /* [0, 11] */
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

int timecalc_weekday(int32_t days)
{
    /* 1970-01-01 was a Thursday. */
    int w = (int)(((int64_t)days + 4) % 7);
    return w < 0 ? w + 7 : w;
}

const char *timecalc_month_abbr(int m)
{
    static const char *const names[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    if (m < 1 || m > 12) return "???";
    return names[m - 1];
}
