#include "ud_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void begin_thinking(usagedata_t *d, ud_host_t *h, int64_t now_us)
{
    (void)d; (void)h; (void)now_us;
    memset(&h->thinking, 0, sizeof h->thinking);
}

static void line_thinking(usagedata_t *d, ud_host_t *h, const char *tag, const char *q)
{
    (void)d; (void)h;
    ud_thinking_t *th = &h->thinking;
    char num[24];
    if (strcmp(tag, "grid") == 0) {
        ud_token(q, th->grid, UD_MDAYS);
    } else if (strcmp(tag, "m") == 0) {
        if (th->model_count >= UD_MAX_MODELS) return;
        ud_think_model_t m;
        memset(&m, 0, sizeof m);
        q = ud_token(q, m.name, UD_NAME_MAX);
        if (q == NULL) return;
        uint64_t *fields[4] = { &m.thinking, &m.visible, &m.think_cents, &m.out_cents };
        int got = 0;
        for (int f = 0; f < 4 && q; f++) {
            q = ud_token(q, num, sizeof num - 1);
            if (q == NULL) break;
            *fields[f] = strtoull(num, NULL, 10);
            got++;
        }
        if (got < 2) return;
        th->models[th->model_count++] = m;
    } else if (ud_token(q, num, sizeof num - 1) != NULL) {
        if (strcmp(tag, "days") == 0) th->days = atoi(num);
        else if (strcmp(tag, "start") == 0) th->start = (int32_t)strtol(num, NULL, 10);
        else if (strcmp(tag, "today") == 0) th->today = (int32_t)strtol(num, NULL, 10);
    }
}

static void end_thinking(usagedata_t *d, ud_host_t *h)
{
    (void)d;
    ud_thinking_t *th = &h->thinking;
    int32_t len = th->today - th->start + 1;
    th->used = th->model_count > 0;
    if (th->today <= 0 || len < 1 || len > UD_MDAYS) { th->today = 0; th->grid[0] = '\0'; }
}

static void merge_thinking(const usagedata_t *d, ud_view_t *out)
{
    ud_thinking_view_t *v = &out->thinking;
    memset(v, 0, sizeof *v);
    for (int i = 0; i < UD_MDAYS; i++) v->pct[i] = -1;
    int32_t starts[UD_MAX_HOSTS] = {0}, todays[UD_MAX_HOSTS] = {0};
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->thinking.used) continue;
        starts[i] = h->thinking.start;
        todays[i] = h->thinking.today;
    }
    ud_pick_window(starts, todays, UD_MAX_HOSTS, &v->start, &v->today, &v->len);

    int sum[UD_MDAYS] = {0}, n[UD_MDAYS] = {0};
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->thinking.used) continue;
        const ud_thinking_t *th = &h->thinking;
        v->present = true;
        if (th->days > v->days) v->days = th->days;
        for (int m = 0; m < th->model_count; m++) {
            const ud_think_model_t *tm = &th->models[m];
            v->thinking += tm->thinking;
            v->visible += tm->visible;
            v->think_cents += tm->think_cents;
            v->out_cents += tm->out_cents;
            int j;
            for (j = 0; j < v->model_count; j++)
                if (strcmp(v->models[j].name, tm->name) == 0) break;
            if (j == v->model_count) {
                if (v->model_count >= UD_MAX_MODELS) continue;
                v->models[v->model_count++] = *tm;
            } else {
                v->models[j].thinking += tm->thinking;
                v->models[j].visible += tm->visible;
                v->models[j].think_cents += tm->think_cents;
                v->models[j].out_cents += tm->out_cents;
            }
        }
        if (th->today <= 0 || v->len == 0) continue;
        for (int c = 0; th->grid[c] != '\0' && c < UD_MDAYS; c++) {
            int32_t idx = th->start + c - v->start;
            int pct = usagedata_pct_value(th->grid[c]);
            if (idx < 0 || idx >= v->len || pct < 0) continue;
            sum[idx] += pct;
            n[idx]++;
        }
    }
    for (int i = 0; i < v->len; i++)
        if (n[i] > 0) v->pct[i] = (int8_t)(sum[i] / n[i]);
    uint64_t total_out = v->thinking + v->visible;
    v->share_pct = total_out ? (int)((v->thinking * 100 + total_out / 2) / total_out) : 0;
    for (int i = 1; i < v->model_count; i++) {
        ud_think_model_t key = v->models[i];
        uint64_t kt = key.thinking + key.visible;
        int j = i - 1;
        while (j >= 0 && v->models[j].thinking + v->models[j].visible < kt) {
            v->models[j + 1] = v->models[j];
            j--;
        }
        v->models[j + 1] = key;
    }
}

static bool used_thinking(const ud_host_t *h)
{
    return h->thinking.used;
}

const ud_section_t ud_section_thinking = {
    "!thinking", UD_THINKING, true,
    begin_thinking, line_thinking, end_thinking, merge_thinking, used_thinking
};
