#include "ud_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void begin_cache(usagedata_t *d, ud_host_t *h, int64_t now_us)
{
    (void)d; (void)h; (void)now_us;
    memset(&h->cache, 0, sizeof h->cache);
}

static void line_cache(usagedata_t *d, ud_host_t *h, const char *tag, const char *q)
{
    (void)d; (void)h;
    ud_cache_t *k = &h->cache;
    char num[24];
    if (strcmp(tag, "grid") == 0) {
        ud_token(q, k->grid, UD_MDAYS);
    } else if (strcmp(tag, "m") == 0) {
        if (k->model_count >= UD_MAX_MODELS) return;
        ud_cache_model_t m;
        memset(&m, 0, sizeof m);
        q = ud_token(q, m.name, UD_NAME_MAX);
        if (q == NULL) return;
        uint64_t *fields[4] = { &m.in, &m.cread, &m.cwrite, &m.saved_cents };
        int got = 0;
        for (int f = 0; f < 4 && q; f++) {
            q = ud_token(q, num, sizeof num - 1);
            if (q == NULL) break;
            *fields[f] = strtoull(num, NULL, 10);
            got++;
        }
        if (got < 3) return;
        k->models[k->model_count++] = m;
    } else if (ud_token(q, num, sizeof num - 1) != NULL) {
        if (strcmp(tag, "start") == 0) k->start = (int32_t)strtol(num, NULL, 10);
        else if (strcmp(tag, "today") == 0) k->today = (int32_t)strtol(num, NULL, 10);
        else if (strcmp(tag, "saved") == 0) k->saved = strtoull(num, NULL, 10);
        else if (strcmp(tag, "cost") == 0) k->cost = strtoull(num, NULL, 10);
    }
}

static void end_cache(usagedata_t *d, ud_host_t *h)
{
    (void)d;
    ud_cache_t *k = &h->cache;
    int32_t len = k->today - k->start + 1;
    k->used = k->model_count > 0
           && (k->today <= 0 || (len >= 1 && len <= UD_MDAYS));
    if (k->today <= 0) k->grid[0] = '\0';      /* no dates, no daily line */
}

/* Cache use: models merged by name; daily percentages averaged by date. */
static void merge_cache(const usagedata_t *d, ud_view_t *out)
{
    ud_cache_view_t *v = &out->cache;
    memset(v, 0, sizeof *v);
    for (int i = 0; i < UD_MDAYS; i++) v->pct[i] = -1;

    const ud_cache_t *anchor = NULL;
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->cache.used) continue;
        v->present = true;
        if (h->cache.today > 0 && (anchor == NULL || h->cache.today > anchor->today))
            anchor = &h->cache;
    }
    if (!v->present) return;
    if (anchor) {
        v->start = anchor->start;
        v->today = anchor->today;
        v->len = (int)(anchor->today - anchor->start + 1);
    }

    int sum[UD_MDAYS] = {0}, n[UD_MDAYS] = {0};
    uint64_t in = 0, cread = 0, cwrite = 0;
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->cache.used) continue;
        const ud_cache_t *k = &h->cache;
        v->saved += k->saved;
        v->cost += k->cost;
        for (int m = 0; m < k->model_count; m++) {
            const ud_cache_model_t *cm = &k->models[m];
            in += cm->in; cread += cm->cread; cwrite += cm->cwrite;
            int j;
            for (j = 0; j < v->model_count; j++)
                if (strcmp(v->models[j].name, cm->name) == 0) break;
            if (j == v->model_count) {
                if (v->model_count >= UD_MAX_MODELS) continue;
                v->models[v->model_count++] = *cm;
            } else {
                v->models[j].in += cm->in;
                v->models[j].cread += cm->cread;
                v->models[j].cwrite += cm->cwrite;
                v->models[j].saved_cents += cm->saved_cents;
            }
        }
        if (k->today <= 0 || v->len == 0) continue;
        for (int c = 0; k->grid[c] != '\0' && c < UD_MDAYS; c++) {
            int32_t idx = k->start + c - v->start;
            int pct = usagedata_pct_value(k->grid[c]);
            if (idx < 0 || idx >= v->len || pct < 0) continue;
            sum[idx] += pct;
            n[idx]++;
        }
    }
    for (int i = 0; i < v->len; i++)
        if (n[i] > 0) v->pct[i] = (int8_t)(sum[i] / n[i]);
    uint64_t total = in + cread + cwrite;
    v->hit_pct = total ? (int)((cread * 100 + total / 2) / total) : 0;

    /* Largest input volume first. */
    for (int i = 1; i < v->model_count; i++) {
        ud_cache_model_t key = v->models[i];
        uint64_t kt = key.in + key.cread + key.cwrite;
        int j = i - 1;
        while (j >= 0 && v->models[j].in + v->models[j].cread + v->models[j].cwrite < kt) {
            v->models[j + 1] = v->models[j];
            j--;
        }
        v->models[j + 1] = key;
    }
}

static bool used_cache(const ud_host_t *h)
{
    return h->cache.used;
}

const ud_section_t ud_section_cache = {
    "!cache", UD_CACHE, true,
    begin_cache, line_cache, end_cache, merge_cache, used_cache
};
