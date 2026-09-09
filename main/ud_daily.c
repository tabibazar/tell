#include "ud_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void add_day(ud_view_t *v, const ud_day_t *day, int host)
{
    for (int i = 0; i < v->day_count; i++) {
        if (strcmp(v->days[i].label, day->label) == 0) {
            v->days[i].tokens += day->tokens;
            v->day_by_host[i][host] += day->tokens;
            return;
        }
    }
    if (v->day_count >= UD_MAX_DAYS) return;
    int i = v->day_count++;
    v->days[i] = *day;
    v->day_by_host[i][host] = day->tokens;
}

static void merge_daily(const usagedata_t *d, ud_view_t *out)
{
    const ud_host_t *hosts[UD_MAX_HOSTS];
    int n = ud_host_order(d, hosts);
    for (int host = 0; host < n; host++)
        for (int k = 0; k < hosts[host]->day_count; k++) add_day(out, &hosts[host]->days[k], host);

    /* Days oldest first: the labels are MM-DD, which sorts correctly. */
    for (int i = 1; i < out->day_count; i++) {
        ud_day_t key = out->days[i];
        uint64_t share[UD_MAX_HOSTS];
        memcpy(share, out->day_by_host[i], sizeof share);
        int j = i - 1;
        while (j >= 0 && strcmp(out->days[j].label, key.label) > 0) {
            out->days[j + 1] = out->days[j];
            memcpy(out->day_by_host[j + 1], out->day_by_host[j], sizeof share);
            j--;
        }
        out->days[j + 1] = key;
        memcpy(out->day_by_host[j + 1], share, sizeof share);
    }
}

static void begin_daily(usagedata_t *d, ud_host_t *h, int64_t now_us)
{
    (void)d; (void)h; (void)now_us;
    h->day_count = 0;
}

static void line_daily(usagedata_t *d, ud_host_t *h, const char *tag, const char *q)
{
    (void)d; (void)h;
    if (strcmp(tag, "d") != 0) return;
    if (h->day_count >= UD_MAX_DAYS) return;
    ud_day_t day;
    char num[24];
    q = ud_token(q, day.label, UD_LABEL_MAX);
    if (q == NULL) return;
    q = ud_token(q, num, sizeof num - 1);
    if (q == NULL) return;
    day.tokens = strtoull(num, NULL, 10);
    /* Optional weekday, so the chart can colour by it. Older senders
       omit it and the day simply has no weekday colour. */
    day.dow = -1;
    if (ud_token(q, num, sizeof num - 1) != NULL) {
        int v = atoi(num);
        if (v >= 0 && v <= 6) day.dow = (int8_t)v;
    }
    h->days[h->day_count++] = day;
}

static bool used_daily(const ud_host_t *h)
{
    return h->day_count > 0;
}

const ud_section_t ud_section_daily = {
    "!daily", UD_DAILY, true,
    begin_daily, line_daily, NULL, merge_daily, used_daily
};
