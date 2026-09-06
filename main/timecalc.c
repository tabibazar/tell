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
