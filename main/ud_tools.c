#include "ud_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void begin_tools(usagedata_t *d, ud_host_t *h, int64_t now_us)
{
    (void)d; (void)h; (void)now_us;
    memset(&h->tools, 0, sizeof h->tools);
}

static void line_tools(usagedata_t *d, ud_host_t *h, const char *tag, const char *q)
{
    (void)d; (void)h;
    ud_tools_t *tl = &h->tools;
    char num[24];
    if (strcmp(tag, "grid") == 0) {
        ud_token(q, tl->grid, UD_MDAYS);
    } else if (strcmp(tag, "t") == 0) {
        if (tl->count >= UD_MAX_MODELS) return;
        ud_tool_t row;
        memset(&row, 0, sizeof row);
        q = ud_token(q, row.name, UD_NAME_MAX);
        if (q == NULL || ud_token(q, num, sizeof num - 1) == NULL) return;
        row.calls = (uint32_t)strtoul(num, NULL, 10);
        tl->rows[tl->count++] = row;
    } else if (ud_token(q, num, sizeof num - 1) != NULL) {
        if (strcmp(tag, "days") == 0) tl->days = atoi(num);
        else if (strcmp(tag, "calls") == 0) tl->calls = (uint32_t)strtoul(num, NULL, 10);
        else if (strcmp(tag, "msgs") == 0) tl->msgs = (uint32_t)strtoul(num, NULL, 10);
        else if (strcmp(tag, "sessions") == 0) tl->sessions = (uint32_t)strtoul(num, NULL, 10);
        else if (strcmp(tag, "start") == 0) tl->start = (int32_t)strtol(num, NULL, 10);
        else if (strcmp(tag, "today") == 0) tl->today = (int32_t)strtol(num, NULL, 10);
    }
}

static void end_tools(usagedata_t *d, ud_host_t *h)
{
    (void)d;
    ud_tools_t *tl = &h->tools;
    int32_t len = tl->today - tl->start + 1;
    tl->used = tl->count > 0;
    if (tl->today <= 0 || len < 1 || len > UD_MDAYS) { tl->today = 0; tl->grid[0] = '\0'; }
}

static void merge_tools(const usagedata_t *d, ud_view_t *out)
{
    ud_tools_view_t *v = &out->tools;
    memset(v, 0, sizeof *v);
    int32_t starts[UD_MAX_HOSTS] = {0}, todays[UD_MAX_HOSTS] = {0};
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->tools.used) continue;
        starts[i] = h->tools.start;
        todays[i] = h->tools.today;
    }
    ud_pick_window(starts, todays, UD_MAX_HOSTS, &v->start, &v->today, &v->len);

    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->tools.used) continue;
        const ud_tools_t *tl = &h->tools;
        v->present = true;
        if (tl->days > v->days) v->days = tl->days;
        v->calls += tl->calls;
        v->msgs += tl->msgs;
        v->sessions += tl->sessions;
        for (int r = 0; r < tl->count; r++) {
            int j;
            for (j = 0; j < v->count; j++)
                if (strcmp(v->rows[j].name, tl->rows[r].name) == 0) break;
            if (j == v->count) {
                if (v->count >= UD_MAX_MODELS) continue;
                v->rows[v->count++] = tl->rows[r];
            } else {
                v->rows[j].calls += tl->rows[r].calls;
            }
        }
        if (tl->today <= 0 || v->len == 0) continue;
        for (int c = 0; tl->grid[c] != '\0' && c < UD_MDAYS; c++) {
            int32_t idx = tl->start + c - v->start;
            if (idx < 0 || idx >= v->len) continue;
            v->day[idx] += usagedata_grid_value(tl->grid[c]) / 1000;
        }
    }
    for (int i = 1; i < v->count; i++) {
        ud_tool_t key = v->rows[i];
        int j = i - 1;
        while (j >= 0 && v->rows[j].calls < key.calls) { v->rows[j + 1] = v->rows[j]; j--; }
        v->rows[j + 1] = key;
    }
}

static bool used_tools(const ud_host_t *h)
{
    return h->tools.used;
}

const ud_section_t ud_section_tools = {
    "!tools", UD_TOOLS, true,
    begin_tools, line_tools, end_tools, merge_tools, used_tools
};
