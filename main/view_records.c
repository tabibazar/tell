#include "view_common.h"

#include <stdio.h>
#include <string.h>

void views_records(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    (void)t;
    canvas_clear(c);
    const ud_records_view_t *r = &d->records;
    char head[72], fresh[24];
    vw_freshness(d, now_us, fresh, sizeof fresh);
    if (r->present && r->since > 0) {
        char since[24];
        vw_full_date(r->since, since, sizeof since);
        snprintf(head, sizeof head, "since %s   %s", since, fresh);
    } else {
        snprintf(head, sizeof head, "%s", fresh);
    }
    vw_title(c, "RECORDS", head);
    if (!r->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    /* Each record: what it is, the value, and when. Unknown keys are ignored,
       so a newer collector can send more than this firmware shows. */
    struct { const char *key, *label; int kind; } rows[] = {
        { "bigday",   "Biggest day",               0 },   /* tokens */
        { "costday",  "Most expensive day",        1 },   /* cents */
        { "msgs",     "Most messages in a day",    2 },   /* count */
        { "streak",   "Longest streak",            3 },   /* days */
        { "session",  "Longest session",           4 },   /* seconds */
        { "toolsess", "Most tool calls, one session", 2 },
        { "response", "Biggest single response",   5 },   /* tokens, "tokens" */
        { "early",    "Earliest message",          6 },   /* minute of day */
        { "late",     "Latest message",            6 },
    };
    const uint16_t hi = pal_heat(PAL_HEAT_STEPS);
    int row = 2;
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        const ud_record_t *rec = usagedata_record(r, rows[i].key);
        if (rec == NULL) continue;
        char val[32], when[24];
        switch (rows[i].kind) {
        case 0: vw_human(rec->value, val, sizeof val); strncat(val, " tokens", sizeof val - strlen(val) - 1); break;
        case 1: vw_money(rec->value, val, sizeof val); break;
        case 3: snprintf(val, sizeof val, "%llu days", (unsigned long long)rec->value); break;
        case 4: vw_duration((uint32_t)rec->value, val, sizeof val); break;
        case 5: vw_human(rec->value, val, sizeof val); strncat(val, " tokens", sizeof val - strlen(val) - 1); break;
        case 6: snprintf(val, sizeof val, "%02llu:%02llu", (unsigned long long)rec->value / 60,
                         (unsigned long long)rec->value % 60); break;
        default: snprintf(val, sizeof val, "%llu", (unsigned long long)rec->value); break;
        }
        canvas_puts(c, 1, row, rows[i].label, PAL_DIM);
        canvas_puts(c, 31, row, val, hi);
        if (rec->day > 0) {
            vw_full_date(rec->day, when, sizeof when);
            if (strcmp(rows[i].key, "streak") == 0) {
                char ending[32];
                snprintf(ending, sizeof ending, "ending %s", when);
                vw_right_text(c, row, c->cols - 1, ending, PAL_DIM);
            } else {
                vw_right_text(c, row, c->cols - 1, when, PAL_DIM);
            }
        }
        row += 2;
        if (row >= c->rows - 1) break;
    }

    char note[96];
    snprintf(note, sizeof note, "personal bests from the transcripts and Claude Code's cache");
    vw_footer(c, note);
}
