#include "ud_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void begin_year(usagedata_t *d, ud_host_t *h, int64_t now_us)
{
    (void)d; (void)h; (void)now_us;
    memset(&h->year, 0, sizeof h->year);
}

static void line_year(usagedata_t *d, ud_host_t *h, const char *tag, const char *q)
{
    (void)d; (void)h;
    ud_year_t *y = &h->year;
    char num[24];
    if (strcmp(tag, "grid") == 0) {
        ud_token(q, y->grid, UD_YEAR_MAX);
    } else if (strcmp(tag, "start") == 0 || strcmp(tag, "today") == 0
               || strcmp(tag, "first") == 0) {
        if (ud_token(q, num, sizeof num - 1) == NULL) return;
        int32_t v = (int32_t)strtol(num, NULL, 10);
        if (tag[0] == 's') y->start = v;
        else if (tag[0] == 't') y->today = v;
        else y->first = v;
    } else if (strcmp(tag, "sessions") == 0
               || strcmp(tag, "longest") == 0) {
        if (ud_token(q, num, sizeof num - 1) == NULL) return;
        uint32_t v = (uint32_t)strtoul(num, NULL, 10);
        if (tag[0] == 's') y->sessions = v;
        else y->longest_secs = v;
    } else if (strcmp(tag, "fav") == 0) {
        ud_token(q, y->fav, UD_NAME_MAX);
    } else if (strcmp(tag, "estimated") == 0) {
        if (ud_token(q, num, sizeof num - 1) != NULL) y->estimated = atoi(num);
    } else if (strcmp(tag, "tok") == 0) {
        for (int k = 0; k < 4; k++) {
            q = ud_token(q, num, sizeof num - 1);
            if (q == NULL) break;
            y->tok[k] = strtoull(num, NULL, 10);
        }
    }
}

static void end_year(usagedata_t *d, ud_host_t *h)
{
    (void)d;
    /* A grid that does not line up with its dates would be drawn in the
       wrong place, so it is dropped rather than guessed at. */
    ud_year_t *y = &h->year;
    int32_t len = y->today - y->start + 1;
    y->used = y->today > 0 && len >= 1 && len <= UD_YEAR_MAX;
}

static void merge_year(const usagedata_t *d, ud_view_t *out)
{
    ud_year_view_t *y = &out->year;
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

    y->active_days = ud_rank_levels(tokens, y->len, y->level, sorted);

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

static bool used_year(const ud_host_t *h)
{
    return h->year.used;
}

const ud_section_t ud_section_year = {
    "!year", UD_YEAR, true,
    begin_year, line_year, end_year, merge_year, used_year
};
