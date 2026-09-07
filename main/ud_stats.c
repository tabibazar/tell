#include "ud_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void add_model(ud_view_t *v, const ud_model_t *m, int host,
                      int32_t hstart)
{
    int i;
    for (i = 0; i < v->model_count; i++)
        if (strcmp(v->models[i].name, m->name) == 0) break;
    if (i == v->model_count) {
        if (v->model_count >= UD_MAX_MODELS) return;
        v->model_count++;
        v->models[i] = *m;
        v->models[i].out = v->models[i].cread = v->models[i].in = 0;
        v->models[i].cwrite = 0;
        v->models[i].calls = 0;
        v->models[i].grid[0] = '\0';
    }
    v->models[i].out += m->out;
    v->models[i].cread += m->cread;
    v->models[i].in += m->in;
    v->models[i].cwrite += m->cwrite;
    v->models[i].calls += m->calls;
    v->model_by_host[i][host] += m->out + m->cread;

    if (hstart <= 0 || v->mlen == 0) return;
    for (int k = 0; m->grid[k] != '\0' && k < UD_MDAYS; k++) {
        int32_t idx = hstart + k - v->mstart;
        if (idx < 0 || idx >= v->mlen) continue;
        v->model_day[i][idx] += usagedata_grid_value(m->grid[k]);
    }
}

static void merge_stats(const usagedata_t *d, ud_view_t *out)
{
    /* The model grids' window: the most recently dated machine's, as long as
       it is sane. Chosen before the rows are added so they can be placed. */
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!ud_contributes(h) || h->model_count == 0 || h->mtoday <= 0) continue;
        int32_t len = h->mtoday - h->mstart + 1;
        if (len < 1 || len > UD_MDAYS) continue;
        if (h->mtoday > out->mtoday) {
            out->mstart = h->mstart;
            out->mtoday = h->mtoday;
            out->mlen = (int)len;
        }
    }

    const ud_host_t *hosts[UD_MAX_HOSTS];
    int n = ud_host_order(d, hosts);
    for (int host = 0; host < n; host++) {
        const ud_host_t *h = hosts[host];
        int32_t hstart = h->mtoday > 0 ? h->mstart : 0;
        for (int m = 0; m < h->model_count; m++)
            add_model(out, &h->models[m], host, hstart);
    }

    /* Models largest first, so the bars rank sensibly after summing. The
       per-host shares move with their row. */
    for (int i = 1; i < out->model_count; i++) {
        ud_model_t key = out->models[i];
        uint64_t share[UD_MAX_HOSTS];
        uint64_t days[UD_MDAYS];
        memcpy(share, out->model_by_host[i], sizeof share);
        memcpy(days, out->model_day[i], sizeof days);
        uint64_t kt = key.out + key.cread;
        int j = i - 1;
        while (j >= 0 && out->models[j].out + out->models[j].cread < kt) {
            out->models[j + 1] = out->models[j];
            memcpy(out->model_by_host[j + 1], out->model_by_host[j],
                   sizeof share);
            memcpy(out->model_day[j + 1], out->model_day[j], sizeof days);
            j--;
        }
        out->models[j + 1] = key;
        memcpy(out->model_by_host[j + 1], share, sizeof share);
        memcpy(out->model_day[j + 1], days, sizeof days);
    }
}

static void begin_stats(usagedata_t *d, ud_host_t *h, int64_t now_us)
{
    (void)d; (void)h; (void)now_us;
    h->model_count = 0; h->mstart = h->mtoday = 0;
}

static void line_stats(usagedata_t *d, ud_host_t *h, const char *tag, const char *q)
{
    (void)d; (void)h;
    if ((strcmp(tag, "start") == 0 || strcmp(tag, "today") == 0)) {
        char num[24];
        if (ud_token(q, num, sizeof num - 1) == NULL) return;
        int32_t v = (int32_t)strtol(num, NULL, 10);
        if (tag[0] == 's') h->mstart = v; else h->mtoday = v;
    } else if (strcmp(tag, "m") == 0) {
        if (h->model_count >= UD_MAX_MODELS) return;
        ud_model_t m;
        memset(&m, 0, sizeof m);
        char num[24];
        q = ud_token(q, m.name, UD_NAME_MAX);
        if (q == NULL) return;
        q = ud_token(q, num, sizeof num - 1);
        if (q == NULL) return;
        m.out = strtoull(num, NULL, 10);
        q = ud_token(q, num, sizeof num - 1);
        m.cread = (q == NULL) ? 0 : strtoull(num, NULL, 10);
        /* The rest is optional, so a row from an older client still
           counts; each field is only read if the one before it was. */
        if (q && (q = ud_token(q, num, sizeof num - 1)) != NULL)
            m.in = strtoull(num, NULL, 10);
        if (q && (q = ud_token(q, num, sizeof num - 1)) != NULL)
            m.cwrite = strtoull(num, NULL, 10);
        if (q && (q = ud_token(q, num, sizeof num - 1)) != NULL)
            m.calls = (uint32_t)strtoul(num, NULL, 10);
        if (q) ud_token(q, m.grid, UD_MDAYS);
        /* Incremented last, so an abandoned row leaves no trace. */
        h->models[h->model_count++] = m;
    }
}

static bool used_stats(const ud_host_t *h)
{
    return h->model_count > 0;
}

const ud_section_t ud_section_stats = {
    "!stats", UD_STATS, true,
    begin_stats, line_stats, NULL, merge_stats, used_stats
};
