#include "ud_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void begin_week(usagedata_t *d, ud_host_t *h, int64_t now_us)
{
    (void)d; (void)h; (void)now_us;
    memset(&h->week, 0, sizeof h->week);
}

static void line_week(usagedata_t *d, ud_host_t *h, const char *tag, const char *q)
{
    (void)d; (void)h;
    ud_week_t *w = &h->week;
    char num[24], key[12];
    if (strcmp(tag, "grid") == 0) {
        ud_token(q, w->grid, 14);
    } else if (strcmp(tag, "today") == 0) {
        if (ud_token(q, num, sizeof num - 1) != NULL) w->today = (int32_t)strtol(num, NULL, 10);
    } else if (strcmp(tag, "w") == 0) {
        static const char *const keys[UD_WEEK_ROWS] = {
            "tokens", "cost", "msgs", "sessions", "tools", "days"
        };
        q = ud_token(q, key, sizeof key - 1);
        if (q == NULL) return;
        int row = -1;
        for (int i = 0; i < UD_WEEK_ROWS; i++) if (strcmp(key, keys[i]) == 0) row = i;
        if (row < 0) return;
        q = ud_token(q, num, sizeof num - 1);
        if (q == NULL) return;
        w->this_week[row] = strtoull(num, NULL, 10);
        if (ud_token(q, num, sizeof num - 1) != NULL) w->last_week[row] = strtoull(num, NULL, 10);
        w->used = true;
    }
}

static void merge_week(const usagedata_t *d, ud_view_t *out)
{
    ud_week_view_t *v = &out->week;
    memset(v, 0, sizeof *v);
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->week.used) continue;
        v->present = true;
        if (h->week.today > v->today) v->today = h->week.today;
        for (int r = 0; r < UD_WEEK_ROWS; r++) {
            v->this_week[r] += h->week.this_week[r];
            v->last_week[r] += h->week.last_week[r];
        }
    }
    /* Active days cannot exceed seven however many machines were busy. */
    if (v->this_week[5] > 7) v->this_week[5] = 7;
    if (v->last_week[5] > 7) v->last_week[5] = 7;
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->week.used || h->week.today <= 0) continue;
        /* Line the fourteen days up on the newest today. */
        int shift = (int)(v->today - h->week.today);
        for (int c = 0; h->week.grid[c] != '\0' && c < 14; c++) {
            int idx = c - shift;
            if (idx >= 0 && idx < 14) v->day[idx] += usagedata_grid_value(h->week.grid[c]);
        }
    }
}

static bool used_week(const ud_host_t *h)
{
    return h->week.used;
}

const ud_section_t ud_section_week = {
    "!week", UD_WEEK, true,
    begin_week, line_week, NULL, merge_week, used_week
};
