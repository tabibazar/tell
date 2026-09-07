#include "ud_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void begin_rhythm(usagedata_t *d, ud_host_t *h, int64_t now_us)
{
    (void)d; (void)h; (void)now_us;
    memset(&h->rhythm, 0, sizeof h->rhythm);
}

static void line_rhythm(usagedata_t *d, ud_host_t *h, const char *tag, const char *q)
{
    (void)d; (void)h;
    ud_rhythm_t *r = &h->rhythm;
    char num[24];
    if (strcmp(tag, "grid") == 0) {
        ud_token(q, r->grid, UD_RHYTHM_CELLS);
    } else if (ud_token(q, num, sizeof num - 1) != NULL) {
        if (strcmp(tag, "days") == 0) r->days = atoi(num);
        else if (strcmp(tag, "msgs") == 0) r->msgs = (uint32_t)strtoul(num, NULL, 10);
    }
}

static void end_rhythm(usagedata_t *d, ud_host_t *h)
{
    (void)d;
    h->rhythm.used = h->rhythm.grid[0] != '\0';
}

static void merge_rhythm(const usagedata_t *d, ud_view_t *out)
{
    ud_rhythm_view_t *r = &out->rhythm;
    static uint64_t vals[UD_RHYTHM_CELLS], scratch[UD_RHYTHM_CELLS];
    memset(r, 0, sizeof *r);
    memset(vals, 0, sizeof vals);

    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->rhythm.used) continue;
        r->present = true;
        if (h->rhythm.days > r->days) r->days = h->rhythm.days;
        r->msgs += h->rhythm.msgs;
        for (int c = 0; h->rhythm.grid[c] != '\0' && c < UD_RHYTHM_CELLS; c++)
            vals[c] += usagedata_grid_value(h->rhythm.grid[c]) / 1000;   /* a thousand is one message */
    }
    if (!r->present) return;

    ud_rank_levels(vals, UD_RHYTHM_CELLS, r->level, scratch);

    uint64_t total = 0, night = 0, weekend = 0, peak = 0;
    uint64_t by_dow[7] = {0}, by_hour[24] = {0};
    for (int c = 0; c < UD_RHYTHM_CELLS; c++) {
        int dow = c / 24, hour = c % 24;
        uint64_t v = vals[c];
        r->cell[c] = (uint32_t)v;
        total += v;
        by_dow[dow] += v;
        by_hour[hour] += v;
        if (hour >= 22 || hour < 6) night += v;
        if (dow >= 5) weekend += v;
        if (v > peak) { peak = v; r->peak_dow = dow; r->peak_hour = hour; }
    }
    for (int i = 1; i < 7; i++) if (by_dow[i] > by_dow[r->busiest_dow]) r->busiest_dow = i;
    for (int i = 1; i < 24; i++) if (by_hour[i] > by_hour[r->busiest_hour]) r->busiest_hour = i;
    if (total > 0) {
        r->night_pct = (int)((night * 100 + total / 2) / total);
        r->weekend_pct = (int)((weekend * 100 + total / 2) / total);
    }
    if (r->msgs == 0) r->msgs = total;
}

static bool used_rhythm(const ud_host_t *h)
{
    return h->rhythm.used;
}

const ud_section_t ud_section_rhythm = {
    "!rhythm", UD_RHYTHM, true,
    begin_rhythm, line_rhythm, end_rhythm, merge_rhythm, used_rhythm
};
