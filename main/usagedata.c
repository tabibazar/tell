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

/* The slot for a machine, reusing its existing one so a re-push replaces
   rather than accumulates. Falls back to the last slot when full, which is
   better than dropping the newest machine silently. */
static ud_host_t *host_slot(usagedata_t *d, const char *name)
{
    for (int i = 0; i < UD_MAX_HOSTS; i++)
        if (d->hosts[i].used && strcmp(d->hosts[i].host, name) == 0)
            return &d->hosts[i];

    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        if (!d->hosts[i].used) {
            memset(&d->hosts[i], 0, sizeof d->hosts[i]);
            strncpy(d->hosts[i].host, name, UD_HOST_MAX);
            d->hosts[i].used = true;
            return &d->hosts[i];
        }
    }
    return &d->hosts[UD_MAX_HOSTS - 1];
}

/* Scans the payload for its "host <name>" line before parsing rows, since it
   decides which machine's data is being replaced. */
static void find_host(const char *payload, char *out)
{
    strcpy(out, "mac");
    for (const char *p = next_line(payload); *p; p = next_line(p)) {
        char tag[8];
        const char *q = token(p, tag, 7);
        if (q == NULL) continue;
        if (strcmp(tag, "host") == 0 && token(q, out, UD_HOST_MAX) != NULL)
            return;
    }
}

ud_kind_t usagedata_parse(usagedata_t *d, const char *payload,
                          int64_t now_us)
{
    if (d == NULL || payload == NULL) return UD_NONE;

    ud_kind_t kind;
    if (starts_with(payload, "!stats")) kind = UD_STATS;
    else if (starts_with(payload, "!daily")) kind = UD_DAILY;
    else if (starts_with(payload, "!clock")) kind = UD_CLOCK;
    else if (starts_with(payload, "!today")) kind = UD_TODAY;
    /* Returns straight away: unlike the others this is a request, not
       data, so there is nothing to store. */
    else if (starts_with(payload, "!particles")) return UD_PARTICLES;
    else if (starts_with(payload, "!level")) return UD_LEVEL;
    else if (starts_with(payload, "!game")) return UD_GAME;
    else return UD_NONE;

    if (kind == UD_CLOCK) {
        d->date[0] = '\0';
        d->weather[0] = '\0';
    } else if (kind == UD_TODAY) {
        d->moon_name[0] = d->sun[0] = d->dayinfo[0] = d->holiday[0] = '\0';
        d->moon_phase = 0.0f;
    }

    ud_host_t *h = NULL;
    if (kind == UD_STATS || kind == UD_DAILY) {
        char name[UD_HOST_MAX + 1];
        find_host(payload, name);
        h = host_slot(d, name);
        h->updated_us = now_us;
        /* A section replaces this machine's rows wholesale: no merge, so no
           stale rows, and the other machine's data is untouched. */
        if (kind == UD_STATS) h->model_count = 0;
        else h->day_count = 0;
    }

    for (const char *p = next_line(payload); *p; p = next_line(p)) {
        char tag[8];
        const char *q = token(p, tag, 7);
        if (q == NULL) continue;

        if (kind == UD_STATS && strcmp(tag, "m") == 0) {
            if (h->model_count >= UD_MAX_MODELS) continue;
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
            h->models[h->model_count++] = m;
        } else if (kind == UD_DAILY && strcmp(tag, "d") == 0) {
            if (h->day_count >= UD_MAX_DAYS) continue;
            ud_day_t day;
            char num[24];
            q = token(q, day.label, UD_LABEL_MAX);
            if (q == NULL) continue;
            q = token(q, num, sizeof num - 1);
            if (q == NULL) continue;
            day.tokens = strtoull(num, NULL, 10);
            /* Optional weekday, so the chart can colour by it. Older senders
               omit it and the day simply has no weekday colour. */
            day.dow = -1;
            if (token(q, num, sizeof num - 1) != NULL) {
                int v = atoi(num);
                if (v >= 0 && v <= 6) day.dow = (int8_t)v;
            }
            h->days[h->day_count++] = day;
        } else if (kind == UD_TODAY) {
            char *dest = NULL;
            if (strcmp(tag, "moon") == 0) {
                /* "moon <phase> <name>": a number then free text. */
                char num[16];
                const char *r = token(q, num, sizeof num - 1);
                if (r == NULL) continue;
                d->moon_phase = (float)atof(num);
                q = r;
                dest = d->moon_name;
            } else if (strcmp(tag, "sun") == 0) dest = d->sun;
            else if (strcmp(tag, "day") == 0) dest = d->dayinfo;
            else if (strcmp(tag, "hol") == 0) dest = d->holiday;
            if (dest == NULL) continue;

            while (*q == ' ' || *q == '\t') q++;
            int n = 0;
            while (q[n] && q[n] != '\n' && q[n] != '\r' && n < UD_TEXT_MAX) n++;
            memcpy(dest, q, (size_t)n);
            dest[n] = '\0';
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
        }
    }
    return kind;
}

/* Adds one machine's row, keeping both the total and that machine's share. */
static void add_model(ud_view_t *v, const ud_model_t *m, int host)
{
    for (int i = 0; i < v->model_count; i++) {
        if (strcmp(v->models[i].name, m->name) == 0) {
            v->models[i].out += m->out;
            v->models[i].cread += m->cread;
            v->model_by_host[i][host] += m->out + m->cread;
            return;
        }
    }
    if (v->model_count >= UD_MAX_MODELS) return;
    int i = v->model_count++;
    v->models[i] = *m;
    v->model_by_host[i][host] = m->out + m->cread;
}

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

/* Only machines that have actually sent something are counted, so an empty
   slot never appears in a legend. */
static bool contributes(const ud_host_t *h)
{
    return h->used && (h->model_count || h->day_count);
}

int usagedata_hosts(const usagedata_t *d)
{
    int n = 0;
    for (int i = 0; i < UD_MAX_HOSTS; i++)
        if (contributes(&d->hosts[i])) n++;
    return n;
}

const char *usagedata_host_name(const usagedata_t *d, int which)
{
    int n = 0;
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        if (!contributes(&d->hosts[i])) continue;
        if (n == which) return d->hosts[i].host;
        n++;
    }
    return "";
}

void usagedata_merge(const usagedata_t *d, ud_view_t *out)
{
    memset(out, 0, sizeof *out);

    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!contributes(h)) continue;
        int host = out->host_count++;
        strncpy(out->host_names[host], h->host, UD_HOST_MAX);
        if (h->updated_us > out->updated_us) out->updated_us = h->updated_us;

        for (int m = 0; m < h->model_count; m++) add_model(out, &h->models[m], host);
        for (int k = 0; k < h->day_count; k++) add_day(out, &h->days[k], host);
    }

    /* Models largest first, so the bars rank sensibly after summing. The
       per-host shares move with their row. */
    for (int i = 1; i < out->model_count; i++) {
        ud_model_t key = out->models[i];
        uint64_t share[UD_MAX_HOSTS];
        memcpy(share, out->model_by_host[i], sizeof share);
        uint64_t kt = key.out + key.cread;
        int j = i - 1;
        while (j >= 0 && out->models[j].out + out->models[j].cread < kt) {
            out->models[j + 1] = out->models[j];
            memcpy(out->model_by_host[j + 1], out->model_by_host[j],
                   sizeof share);
            j--;
        }
        out->models[j + 1] = key;
        memcpy(out->model_by_host[j + 1], share, sizeof share);
    }

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
