#include "ud_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void begin_turns(usagedata_t *d, ud_host_t *h, int64_t now_us)
{
    (void)d; (void)h; (void)now_us;
    memset(&h->turns, 0, sizeof h->turns);
}

static void line_turns(usagedata_t *d, ud_host_t *h, const char *tag, const char *q)
{
    (void)d; (void)h;
    ud_turns_t *tn = &h->turns;
    char num[24];
    if (strcmp(tag, "grid") == 0) {
        ud_token(q, tn->grid, UD_MDAYS);
        return;
    }
    q = ud_token(q, num, sizeof num - 1);
    if (q == NULL) return;
    uint64_t a = strtoull(num, NULL, 10), b = 0;
    if (ud_token(q, num, sizeof num - 1) != NULL) b = strtoull(num, NULL, 10);
    if (strcmp(tag, "days") == 0) tn->days = (int)a;
    else if (strcmp(tag, "turns") == 0) { tn->turns = (uint32_t)a; tn->used = true; }
    else if (strcmp(tag, "interrupted") == 0) tn->interrupted = (uint32_t)a;
    else if (strcmp(tag, "first") == 0) { tn->first_med = (uint32_t)a; tn->first_p90 = (uint32_t)b; }
    else if (strcmp(tag, "turn") == 0) { tn->turn_med = (uint32_t)a; tn->turn_p90 = (uint32_t)b; }
    else if (strcmp(tag, "longest") == 0) { tn->longest = (uint32_t)a; tn->longest_day = (int32_t)b; }
    else if (strcmp(tag, "start") == 0) tn->start = (int32_t)a;
    else if (strcmp(tag, "today") == 0) tn->today = (int32_t)a;
}

static void end_turns(usagedata_t *d, ud_host_t *h)
{
    (void)d;
    ud_turns_t *tn = &h->turns;
    int32_t len = tn->today - tn->start + 1;
    if (tn->today <= 0 || len < 1 || len > UD_MDAYS) { tn->today = 0; tn->grid[0] = '\0'; }
}

static void merge_turns(const usagedata_t *d, ud_view_t *out)
{
    ud_turns_view_t *v = &out->turns;
    memset(v, 0, sizeof *v);
    for (int i = 0; i < UD_MDAYS; i++) v->day[i] = -1;
    int32_t starts[UD_MAX_HOSTS] = {0}, todays[UD_MAX_HOSTS] = {0};
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->turns.used) continue;
        starts[i] = h->turns.start;
        todays[i] = h->turns.today;
    }
    ud_pick_window(starts, todays, UD_MAX_HOSTS, &v->start, &v->today, &v->len);

    uint64_t w_first_med = 0, w_first_p90 = 0, w_turn_med = 0, w_turn_p90 = 0;
    int64_t sum[UD_MDAYS] = {0};
    int n[UD_MDAYS] = {0};
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->turns.used) continue;
        const ud_turns_t *tn = &h->turns;
        v->present = true;
        if (tn->days > v->days) v->days = tn->days;
        v->turns += tn->turns;
        v->interrupted += tn->interrupted;
        w_first_med += (uint64_t)tn->first_med * tn->turns;
        w_first_p90 += (uint64_t)tn->first_p90 * tn->turns;
        w_turn_med += (uint64_t)tn->turn_med * tn->turns;
        w_turn_p90 += (uint64_t)tn->turn_p90 * tn->turns;
        if (tn->longest > v->longest) { v->longest = tn->longest; v->longest_day = tn->longest_day; }
        if (tn->today <= 0 || v->len == 0) continue;
        for (int c = 0; tn->grid[c] != '\0' && c < UD_MDAYS; c++) {
            int32_t idx = tn->start + c - v->start;
            if (idx < 0 || idx >= v->len || tn->grid[c] == '.') continue;
            sum[idx] += (int64_t)(usagedata_grid_value(tn->grid[c]) / 1000);
            n[idx]++;
        }
    }
    if (v->turns > 0) {
        v->first_med = (uint32_t)(w_first_med / v->turns);
        v->first_p90 = (uint32_t)(w_first_p90 / v->turns);
        v->turn_med = (uint32_t)(w_turn_med / v->turns);
        v->turn_p90 = (uint32_t)(w_turn_p90 / v->turns);
    }
    for (int i = 0; i < v->len; i++)
        if (n[i] > 0) v->day[i] = sum[i] / n[i];
}

static bool used_turns(const ud_host_t *h)
{
    return h->turns.used;
}

const ud_section_t ud_section_turns = {
    "!turns", UD_TURNS, true,
    begin_turns, line_turns, end_turns, merge_turns, used_turns
};
