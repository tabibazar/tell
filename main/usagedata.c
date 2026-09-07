#include "usagedata.h"

#include <stdlib.h>
#include <string.h>

/* Copies up to `max` chars of a whitespace-delimited token. Returns NULL when
   there was no token, so a caller can abandon a short row. */
static const char *token(const char *p, char *out, int max)
{
    while (*p == ' ' || *p == '\t') p++;
    int n = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        if (n < max) out[n++] = *p;
        p++;
    }
    out[n] = '\0';
    return n ? p : NULL;
}

static const char *next_line(const char *p)
{
    while (*p && *p != '\n') p++;
    return *p ? p + 1 : p;
}

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

ud_kind_t usagedata_parse(usagedata_t *d, const char *payload)
{
    if (d == NULL || payload == NULL) return UD_NONE;

    ud_kind_t kind;
    if (starts_with(payload, "!stats")) kind = UD_STATS;
    else if (starts_with(payload, "!daily")) kind = UD_DAILY;
    else if (starts_with(payload, "!clock")) kind = UD_CLOCK;
    else return UD_NONE;

    /* A section replaces its rows wholesale: no merge, so no stale rows. */
    if (kind == UD_STATS) d->model_count = 0;
    else if (kind == UD_DAILY) d->day_count = 0;
    else { d->date[0] = '\0'; d->weather[0] = '\0'; }

    for (const char *p = next_line(payload); *p; p = next_line(p)) {
        char tag[8];
        const char *q = token(p, tag, 7);
        if (q == NULL) continue;

        if (kind == UD_STATS && tag[0] == 'm' && tag[1] == '\0') {
            if (d->model_count >= UD_MAX_MODELS) continue;
            ud_model_t m;
            char num[24];
            q = token(q, m.name, UD_NAME_MAX);
            if (q == NULL) continue;
            q = token(q, num, sizeof num - 1);
            if (q == NULL) continue;
            m.out = strtoull(num, NULL, 10);
            q = token(q, num, sizeof num - 1);
            m.cread = (q == NULL) ? 0 : strtoull(num, NULL, 10);
            /* Incremented last, so an abandoned row leaves no trace. */
            d->models[d->model_count++] = m;
        } else if (kind == UD_CLOCK) {
            /* The rest of the line is free text, so take it verbatim. */
            char *dest = NULL;
            if (strcmp(tag, "date") == 0) dest = d->date;
            else if (strcmp(tag, "wx") == 0) dest = d->weather;
            if (dest == NULL) continue;

            while (*q == ' ' || *q == '\t') q++;
            int n = 0;
            while (q[n] && q[n] != '\n' && q[n] != '\r' && n < UD_TEXT_MAX) n++;
            memcpy(dest, q, (size_t)n);
            dest[n] = '\0';
        } else if (kind == UD_DAILY && tag[0] == 'd' && tag[1] == '\0') {
            if (d->day_count >= UD_MAX_DAYS) continue;
            ud_day_t day;
            char num[24];
            q = token(q, day.label, UD_LABEL_MAX);
            if (q == NULL) continue;
            q = token(q, num, sizeof num - 1);
            if (q == NULL) continue;
            day.tokens = strtoull(num, NULL, 10);
            d->days[d->day_count++] = day;
        }
    }
    return kind;
}
