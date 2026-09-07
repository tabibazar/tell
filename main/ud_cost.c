#include "ud_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void begin_cost(usagedata_t *d, ud_host_t *h, int64_t now_us)
{
    (void)d; (void)h; (void)now_us;
    memset(&h->cost, 0, sizeof h->cost);
}

static void line_cost(usagedata_t *d, ud_host_t *h, const char *tag, const char *q)
{
    (void)d; (void)h;
    ud_cost_t *k = &h->cost;
    char num[24];
    if (strcmp(tag, "grid") == 0) {
        ud_token(q, k->grid, UD_MDAYS);
    } else if (strcmp(tag, "c") == 0) {
        if (k->model_count >= UD_MAX_MODELS) return;
        ud_cost_model_t m;
        q = ud_token(q, m.name, UD_NAME_MAX);
        if (q == NULL || ud_token(q, num, sizeof num - 1) == NULL) return;
        m.cents = strtoull(num, NULL, 10);
        k->models[k->model_count++] = m;
    } else {
        if (ud_token(q, num, sizeof num - 1) == NULL) return;
        if (strcmp(tag, "start") == 0) k->start = (int32_t)strtol(num, NULL, 10);
        else if (strcmp(tag, "today") == 0) k->today = (int32_t)strtol(num, NULL, 10);
        else if (strcmp(tag, "total") == 0) k->total = strtoull(num, NULL, 10);
        else if (strcmp(tag, "last30") == 0) k->last30 = strtoull(num, NULL, 10);
        else if (strcmp(tag, "last7") == 0) k->last7 = strtoull(num, NULL, 10);
        else if (strcmp(tag, "plan") == 0) k->plan = strtoull(num, NULL, 10);
    }
}

static void end_cost(usagedata_t *d, ud_host_t *h)
{
    (void)d;
    ud_cost_t *k = &h->cost;
    int32_t len = k->today - k->start + 1;
    k->used = k->today > 0 && len >= 1 && len <= UD_MDAYS;
}

/* Sums every machine's cost, lining the days up as merge_year does. */
static void merge_cost(const usagedata_t *d, ud_view_t *out)
{
    ud_cost_view_t *k = &out->cost;
    memset(k, 0, sizeof *k);

    const ud_cost_t *anchor = NULL;
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->cost.used) continue;
        if (anchor == NULL || h->cost.today > anchor->today) anchor = &h->cost;
    }
    if (anchor == NULL) return;

    k->present = true;
    k->start = anchor->start;
    k->today = anchor->today;
    k->len = (int)(anchor->today - anchor->start + 1);

    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->cost.used) continue;
        const ud_cost_t *s = &h->cost;
        k->total += s->total;
        k->last30 += s->last30;
        k->last7 += s->last7;
        /* Every machine is on the same subscription, so the plan is not
           summed; the largest figure wins in case only one machine set it. */
        if (s->plan > k->plan) k->plan = s->plan;

        for (int m = 0; m < s->model_count; m++) {
            int j;
            for (j = 0; j < k->model_count; j++)
                if (strcmp(k->models[j].name, s->models[m].name) == 0) break;
            if (j == k->model_count) {
                if (k->model_count >= UD_MAX_MODELS) continue;
                k->models[k->model_count++] = s->models[m];
            } else {
                k->models[j].cents += s->models[m].cents;
            }
        }
        for (int c = 0; s->grid[c] != '\0' && c < UD_MDAYS; c++) {
            int32_t idx = s->start + c - k->start;
            if (idx < 0 || idx >= k->len) continue;
            k->day[idx] += usagedata_grid_value(s->grid[c]);
        }
    }

    for (int i = 1; i < k->model_count; i++) {
        ud_cost_model_t key = k->models[i];
        int j = i - 1;
        while (j >= 0 && k->models[j].cents < key.cents) {
            k->models[j + 1] = k->models[j];
            j--;
        }
        k->models[j + 1] = key;
    }
}

static bool used_cost(const ud_host_t *h)
{
    return h->cost.used;
}

const ud_section_t ud_section_cost = {
    "!cost", UD_COST, true,
    begin_cost, line_cost, end_cost, merge_cost, used_cost
};
