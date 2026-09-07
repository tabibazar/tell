#include "usagedata.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const char GRID_ALPHABET[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

uint64_t usagedata_grid_value(char c)
{
    const char *p = c ? strchr(GRID_ALPHABET, c) : NULL;
    if (p == NULL) return 0;
    int i = (int)(p - GRID_ALPHABET);
    /* 2^(i/2): a whole power of two, times sqrt(2) for the odd steps. The
       constant is sqrt(2) in 1/65536ths, so no floating point is needed. */
    uint64_t v = 1000ULL << (i / 2);
    if (i & 1) v = (v * 92682ULL) >> 16;
    return v;
}

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
        char tag[12];
        const char *q = token(p, tag, 11);
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
    else if (starts_with(payload, "!year")) kind = UD_YEAR;
    else if (starts_with(payload, "!cost")) kind = UD_COST;
    else return UD_NONE;

    if (kind == UD_CLOCK) {
        d->date[0] = '\0';
        d->weather[0] = '\0';
    } else if (kind == UD_TODAY) {
        d->moon_name[0] = d->sun[0] = d->dayinfo[0] = d->holiday[0] = '\0';
        d->moon_phase = 0.0f;
    }

    ud_host_t *h = NULL;
    if (kind == UD_STATS || kind == UD_DAILY || kind == UD_YEAR
        || kind == UD_COST) {
        char name[UD_HOST_MAX + 1];
        find_host(payload, name);
        h = host_slot(d, name);
        h->updated_us = now_us;
        /* A section replaces this machine's rows wholesale: no merge, so no
           stale rows, and the other machine's data is untouched. */
        if (kind == UD_STATS) { h->model_count = 0; h->mstart = h->mtoday = 0; }
        else if (kind == UD_DAILY) h->day_count = 0;
        else if (kind == UD_YEAR) memset(&h->year, 0, sizeof h->year);
        else memset(&h->cost, 0, sizeof h->cost);
    }

    for (const char *p = next_line(payload); *p; p = next_line(p)) {
        char tag[12];
        const char *q = token(p, tag, 11);
        if (q == NULL) continue;

        if (kind == UD_STATS && (strcmp(tag, "start") == 0
                                 || strcmp(tag, "today") == 0)) {
            char num[24];
            if (token(q, num, sizeof num - 1) == NULL) continue;
            int32_t v = (int32_t)strtol(num, NULL, 10);
            if (tag[0] == 's') h->mstart = v; else h->mtoday = v;
        } else if (kind == UD_STATS && strcmp(tag, "m") == 0) {
            if (h->model_count >= UD_MAX_MODELS) continue;
            ud_model_t m;
            memset(&m, 0, sizeof m);
            char num[24];
            q = token(q, m.name, UD_NAME_MAX);
            if (q == NULL) continue;
            q = token(q, num, sizeof num - 1);
            if (q == NULL) continue;
            m.out = strtoull(num, NULL, 10);
            q = token(q, num, sizeof num - 1);
            m.cread = (q == NULL) ? 0 : strtoull(num, NULL, 10);
            /* The rest is optional, so a row from an older client still
               counts; each field is only read if the one before it was. */
            if (q && (q = token(q, num, sizeof num - 1)) != NULL)
                m.in = strtoull(num, NULL, 10);
            if (q && (q = token(q, num, sizeof num - 1)) != NULL)
                m.cwrite = strtoull(num, NULL, 10);
            if (q && (q = token(q, num, sizeof num - 1)) != NULL)
                m.calls = (uint32_t)strtoul(num, NULL, 10);
            if (q) token(q, m.grid, UD_MDAYS);
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
        } else if (kind == UD_YEAR) {
            ud_year_t *y = &h->year;
            char num[24];
            if (strcmp(tag, "grid") == 0) {
                token(q, y->grid, UD_YEAR_MAX);
            } else if (strcmp(tag, "start") == 0 || strcmp(tag, "today") == 0
                       || strcmp(tag, "first") == 0) {
                if (token(q, num, sizeof num - 1) == NULL) continue;
                int32_t v = (int32_t)strtol(num, NULL, 10);
                if (tag[0] == 's') y->start = v;
                else if (tag[0] == 't') y->today = v;
                else y->first = v;
            } else if (strcmp(tag, "sessions") == 0
                       || strcmp(tag, "longest") == 0) {
                if (token(q, num, sizeof num - 1) == NULL) continue;
                uint32_t v = (uint32_t)strtoul(num, NULL, 10);
                if (tag[0] == 's') y->sessions = v;
                else y->longest_secs = v;
            } else if (strcmp(tag, "fav") == 0) {
                token(q, y->fav, UD_NAME_MAX);
            } else if (strcmp(tag, "estimated") == 0) {
                if (token(q, num, sizeof num - 1) != NULL) y->estimated = atoi(num);
            } else if (strcmp(tag, "tok") == 0) {
                for (int k = 0; k < 4; k++) {
                    q = token(q, num, sizeof num - 1);
                    if (q == NULL) break;
                    y->tok[k] = strtoull(num, NULL, 10);
                }
            }
        } else if (kind == UD_COST) {
            ud_cost_t *k = &h->cost;
            char num[24];
            if (strcmp(tag, "grid") == 0) {
                token(q, k->grid, UD_MDAYS);
            } else if (strcmp(tag, "c") == 0) {
                if (k->model_count >= UD_MAX_MODELS) continue;
                ud_cost_model_t m;
                q = token(q, m.name, UD_NAME_MAX);
                if (q == NULL || token(q, num, sizeof num - 1) == NULL) continue;
                m.cents = strtoull(num, NULL, 10);
                k->models[k->model_count++] = m;
            } else {
                if (token(q, num, sizeof num - 1) == NULL) continue;
                if (strcmp(tag, "start") == 0) k->start = (int32_t)strtol(num, NULL, 10);
                else if (strcmp(tag, "today") == 0) k->today = (int32_t)strtol(num, NULL, 10);
                else if (strcmp(tag, "total") == 0) k->total = strtoull(num, NULL, 10);
                else if (strcmp(tag, "last30") == 0) k->last30 = strtoull(num, NULL, 10);
                else if (strcmp(tag, "last7") == 0) k->last7 = strtoull(num, NULL, 10);
                else if (strcmp(tag, "plan") == 0) k->plan = strtoull(num, NULL, 10);
            }
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
    if (kind == UD_YEAR) {
        /* A grid that does not line up with its dates would be drawn in the
           wrong place, so it is dropped rather than guessed at. */
        ud_year_t *y = &h->year;
        int32_t len = y->today - y->start + 1;
        y->used = y->today > 0 && len >= 1 && len <= UD_YEAR_MAX;
    }
    if (kind == UD_COST) {
        ud_cost_t *k = &h->cost;
        int32_t len = k->today - k->start + 1;
        k->used = k->today > 0 && len >= 1 && len <= UD_MDAYS;
    }
    return kind;
}

/* Adds one machine's row, keeping both the total and that machine's share,
   and lines its days up on the view's window. `hstart` is the day of the
   machine's first grid character; 0 when it sent no grids. */
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
    return h->used && (h->model_count || h->day_count || h->year.used
                       || h->cost.used);
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

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* Lines every machine's grid up by date on the window of whichever machine
   sent most recently, sums the days, and ranks them. */
static void merge_year(const usagedata_t *d, ud_year_view_t *y)
{
    /* Static, not on the stack: the main task's stack is small and a year of
       64-bit totals is 3 KB. Only the main loop merges, so this is safe. */
    static uint64_t tokens[UD_YEAR_MAX];
    static uint64_t sorted[UD_YEAR_MAX];

    memset(y, 0, sizeof *y);
    y->peak_index = -1;

    const ud_year_t *anchor = NULL;
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->year.used) continue;
        if (anchor == NULL || h->year.today > anchor->today) anchor = &h->year;
    }
    if (anchor == NULL) return;

    y->present = true;
    y->start = anchor->start;
    y->today = anchor->today;
    y->len = (int)(anchor->today - anchor->start + 1);
    memset(tokens, 0, sizeof tokens);

    int32_t first = INT32_MAX;
    uint64_t fav_tokens = 0;
    bool have_fav = false;
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->year.used) continue;
        const ud_year_t *s = &h->year;

        uint64_t total = 0;
        for (int k = 0; k < 4; k++) { y->tok[k] += s->tok[k]; total += s->tok[k]; }
        y->sessions += s->sessions;
        if (s->longest_secs > y->longest_secs) y->longest_secs = s->longest_secs;
        if (s->first > 0 && s->first < first) first = s->first;
        if (s->fav[0] && (!have_fav || total > fav_tokens)) {
            strncpy(y->fav, s->fav, UD_NAME_MAX);
            fav_tokens = total;
            have_fav = true;
        }
        y->estimated += s->estimated;

        for (int k = 0; s->grid[k] != '\0' && k < UD_YEAR_MAX; k++) {
            int32_t idx = s->start + k - y->start;
            if (idx < 0 || idx >= y->len) continue;
            tokens[idx] += usagedata_grid_value(s->grid[k]);
        }
    }

    /* Quartiles of the active days, GitHub's rule: a day at or above the
       k-th quarter mark is level k+1, so the single busiest day is always
       the brightest, and one lonely active day is bright rather than faint. */
    int n = 0;
    for (int i = 0; i < y->len; i++)
        if (tokens[i] > 0) sorted[n++] = tokens[i];
    y->active_days = n;
    if (n > 0) {
        qsort(sorted, (size_t)n, sizeof sorted[0], cmp_u64);
        uint64_t q[UD_HEAT_LEVELS - 1];
        for (int k = 1; k < UD_HEAT_LEVELS; k++) {
            int at = k * n / UD_HEAT_LEVELS;
            if (at > n - 1) at = n - 1;
            q[k - 1] = sorted[at];
        }
        for (int i = 0; i < y->len; i++) {
            if (tokens[i] == 0) continue;
            int level = 1;
            for (int k = 0; k < UD_HEAT_LEVELS - 1; k++)
                if (tokens[i] >= q[k]) level = k + 2;
            y->level[i] = (uint8_t)level;
        }
    }

    /* Streaks and the peak, from the merged days. */
    int run = 0;
    uint64_t peak = 0;
    for (int i = 0; i < y->len; i++) {
        if (tokens[i] > 0) {
            run++;
            if (run > y->longest_streak) y->longest_streak = run;
            if (tokens[i] >= peak) { peak = tokens[i]; y->peak_index = i; }
        } else run = 0;
    }
    /* The current streak survives a quiet today: it is still alive until
       midnight passes without a session. */
    int i = y->len - 1;
    if (i >= 0 && tokens[i] == 0) i--;
    while (i >= 0 && tokens[i] > 0) { y->current_streak++; i--; }

    y->span_days = first == INT32_MAX ? y->len : (int)(y->today - first + 1);
    if (y->span_days > y->len) y->span_days = y->len;
    if (y->span_days < y->active_days) y->span_days = y->active_days;
}

/* Sums every machine's cost, lining the days up as merge_year does. */
static void merge_cost(const usagedata_t *d, ud_cost_view_t *k)
{
    memset(k, 0, sizeof *k);

    const ud_cost_t *anchor = NULL;
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->cost.used) continue;
        if (anchor == NULL || h->cost.today > anchor->today) anchor = &h->cost;
    }
    if (anchor == NULL) return;

    k->present = true;
    k->start = anchor->start;
    k->today = anchor->today;
    k->len = (int)(anchor->today - anchor->start + 1);

    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->cost.used) continue;
        const ud_cost_t *s = &h->cost;
        k->total += s->total;
        k->last30 += s->last30;
        k->last7 += s->last7;
        /* Every machine is on the same subscription, so the plan is not
           summed; the largest figure wins in case only one machine set it. */
        if (s->plan > k->plan) k->plan = s->plan;

        for (int m = 0; m < s->model_count; m++) {
            int j;
            for (j = 0; j < k->model_count; j++)
                if (strcmp(k->models[j].name, s->models[m].name) == 0) break;
            if (j == k->model_count) {
                if (k->model_count >= UD_MAX_MODELS) continue;
                k->models[k->model_count++] = s->models[m];
            } else {
                k->models[j].cents += s->models[m].cents;
            }
        }
        for (int c = 0; s->grid[c] != '\0' && c < UD_MDAYS; c++) {
            int32_t idx = s->start + c - k->start;
            if (idx < 0 || idx >= k->len) continue;
            k->day[idx] += usagedata_grid_value(s->grid[c]);
        }
    }

    for (int i = 1; i < k->model_count; i++) {
        ud_cost_model_t key = k->models[i];
        int j = i - 1;
        while (j >= 0 && k->models[j].cents < key.cents) {
            k->models[j + 1] = k->models[j];
            j--;
        }
        k->models[j + 1] = key;
    }
}

void usagedata_merge(const usagedata_t *d, ud_view_t *out)
{
    memset(out, 0, sizeof *out);
    merge_year(d, &out->year);
    merge_cost(d, &out->cost);

    /* The model grids' window: the most recently dated machine's, as long as
       it is sane. Chosen before the rows are added so they can be placed. */
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!contributes(h) || h->model_count == 0 || h->mtoday <= 0) continue;
        int32_t len = h->mtoday - h->mstart + 1;
        if (len < 1 || len > UD_MDAYS) continue;
        if (h->mtoday > out->mtoday) {
            out->mstart = h->mstart;
            out->mtoday = h->mtoday;
            out->mlen = (int)len;
        }
    }

    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!contributes(h)) continue;
        int host = out->host_count++;
        strncpy(out->host_names[host], h->host, UD_HOST_MAX);
        if (h->updated_us > out->updated_us) out->updated_us = h->updated_us;

        int32_t hstart = h->mtoday > 0 ? h->mstart : 0;
        for (int m = 0; m < h->model_count; m++)
            add_model(out, &h->models[m], host, hstart);
        for (int k = 0; k < h->day_count; k++) add_day(out, &h->days[k], host);
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
