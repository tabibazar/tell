#include "ud_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void begin_records(usagedata_t *d, ud_host_t *h, int64_t now_us)
{
    (void)d; (void)h; (void)now_us;
    memset(&h->records, 0, sizeof h->records);
}

static void line_records(usagedata_t *d, ud_host_t *h, const char *tag, const char *q)
{
    (void)d; (void)h;
    ud_records_t *r = &h->records;
    char num[24];
    if (strcmp(tag, "since") == 0) {
        if (ud_token(q, num, sizeof num - 1) != NULL) r->since = (int32_t)strtol(num, NULL, 10);
    } else if (strcmp(tag, "r") == 0) {
        if (r->count >= UD_RECORD_MAX) return;
        ud_record_t row;
        memset(&row, 0, sizeof row);
        q = ud_token(q, row.key, UD_RECORD_KEY);
        if (q == NULL || (q = ud_token(q, num, sizeof num - 1)) == NULL) return;
        row.value = strtoull(num, NULL, 10);
        if (ud_token(q, num, sizeof num - 1) != NULL) row.day = (int32_t)strtol(num, NULL, 10);
        r->rows[r->count++] = row;
        r->used = true;
    }
}

static void merge_records(const usagedata_t *d, ud_view_t *out)
{
    ud_records_view_t *v = &out->records;
    memset(v, 0, sizeof *v);
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->records.used) continue;
        v->present = true;
        if (h->records.since > 0 && (v->since == 0 || h->records.since < v->since))
            v->since = h->records.since;
        for (int r = 0; r < h->records.count; r++) {
            const ud_record_t *row = &h->records.rows[r];
            bool smaller_wins = strcmp(row->key, "early") == 0;
            int j;
            for (j = 0; j < v->count; j++)
                if (strcmp(v->rows[j].key, row->key) == 0) break;
            if (j == v->count) {
                if (v->count >= UD_RECORD_MAX) continue;
                v->rows[v->count++] = *row;
            } else if (smaller_wins ? row->value < v->rows[j].value
                                    : row->value > v->rows[j].value) {
                v->rows[j] = *row;
            }
        }
    }
}

static bool used_records(const ud_host_t *h)
{
    return h->records.used;
}

const ud_section_t ud_section_records = {
    "!records", UD_RECORDS, true,
    begin_records, line_records, NULL, merge_records, used_records
};

const ud_record_t *usagedata_record(const ud_records_view_t *r, const char *key)
{
    for (int i = 0; i < r->count; i++)
        if (strcmp(r->rows[i].key, key) == 0) return &r->rows[i];
    return NULL;
}
