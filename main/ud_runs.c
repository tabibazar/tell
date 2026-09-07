#include "ud_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* In the order of ud_runs_t.cats; the Python side sends these names. */
const char *const usagedata_run_cats[UD_RUN_CATS] = {
    "git", "build", "test", "files", "scripts", "infra", "other"
};

static void begin_runs(usagedata_t *d, ud_host_t *h, int64_t now_us)
{
    (void)d; (void)h; (void)now_us;
    memset(&h->runs, 0, sizeof h->runs);
}

static void line_runs(usagedata_t *d, ud_host_t *h, const char *tag, const char *q)
{
    (void)d; (void)h;
    ud_runs_t *r = &h->runs;
    char num[24], key[16];
    if (strcmp(tag, "c") == 0) {
        if (r->count >= UD_MAX_MODELS + 1) return;
        ud_tool_t row;
        memset(&row, 0, sizeof row);
        q = ud_token(q, row.name, UD_NAME_MAX);
        if (q == NULL || ud_token(q, num, sizeof num - 1) == NULL) return;
        row.calls = (uint32_t)strtoul(num, NULL, 10);
        r->rows[r->count++] = row;
        r->used = true;
    } else if (strcmp(tag, "k") == 0) {
        q = ud_token(q, key, sizeof key - 1);
        if (q == NULL || ud_token(q, num, sizeof num - 1) == NULL) return;
        for (int i = 0; i < UD_RUN_CATS; i++)
            if (strcmp(key, usagedata_run_cats[i]) == 0)
                r->cats[i] = (uint32_t)strtoul(num, NULL, 10);
    } else if (ud_token(q, num, sizeof num - 1) != NULL) {
        if (strcmp(tag, "days") == 0) r->days = atoi(num);
        else if (strcmp(tag, "calls") == 0) r->calls = (uint32_t)strtoul(num, NULL, 10);
        else if (strcmp(tag, "commands") == 0) r->commands = (uint32_t)strtoul(num, NULL, 10);
    }
}

static void merge_runs(const usagedata_t *d, ud_view_t *out)
{
    ud_runs_view_t *v = &out->runs;
    memset(v, 0, sizeof *v);
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->runs.used) continue;
        const ud_runs_t *r = &h->runs;
        v->present = true;
        if (r->days > v->days) v->days = r->days;
        v->calls += r->calls;
        v->commands += r->commands;
        for (int k = 0; k < UD_RUN_CATS; k++) v->cats[k] += r->cats[k];
        for (int m = 0; m < r->count; m++) {
            int j;
            for (j = 0; j < v->count; j++)
                if (strcmp(v->rows[j].name, r->rows[m].name) == 0) break;
            if (j == v->count) {
                if (v->count >= UD_MAX_MODELS + 1) continue;
                v->rows[v->count++] = r->rows[m];
            } else {
                v->rows[j].calls += r->rows[m].calls;
            }
        }
    }
    /* Most run first, but "other" always last however big it is. */
    for (int i = 1; i < v->count; i++) {
        ud_tool_t key = v->rows[i];
        int j = i - 1;
        bool key_other = strcmp(key.name, "other") == 0;
        while (j >= 0 && !key_other
               && (strcmp(v->rows[j].name, "other") == 0 || v->rows[j].calls < key.calls)) {
            v->rows[j + 1] = v->rows[j];
            j--;
        }
        v->rows[j + 1] = key;
    }
}

static bool used_runs(const ud_host_t *h)
{
    return h->runs.used;
}

const ud_section_t ud_section_runs = {
    "!runs", UD_RUNS, true,
    begin_runs, line_runs, NULL, merge_runs, used_runs
};
