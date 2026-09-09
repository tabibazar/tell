#include "ud_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void begin_now(usagedata_t *d, ud_host_t *h, int64_t now_us)
{
    (void)d; (void)h; (void)now_us;
    memset(&h->now, 0, sizeof h->now); h->now_sent_us = now_us;
}

static void line_now(usagedata_t *d, ud_host_t *h, const char *tag, const char *q)
{
    (void)d; (void)h;
    ud_now_t *n = &h->now;
    char num[24];
    if (strcmp(tag, "model") == 0) { ud_token(q, n->model, UD_NAME_MAX); return; }
    if (strcmp(tag, "project") == 0) { ud_token(q, n->project, UD_NAME_MAX); return; }
    if (ud_token(q, num, sizeof num - 1) == NULL) return;
    uint64_t v = strtoull(num, NULL, 10);
    if (strcmp(tag, "tokens") == 0) n->tokens = v;
    else if (strcmp(tag, "avg") == 0) n->avg = v;
    else if (strcmp(tag, "cost") == 0) n->cost = v;
    else if (strcmp(tag, "msgs") == 0) n->msgs = (uint32_t)v;
    else if (strcmp(tag, "sessions") == 0) n->sessions = (uint32_t)v;
    else if (strcmp(tag, "session") == 0) n->session_secs = (uint32_t)v;
    else if (strcmp(tag, "last") == 0) { n->last_secs = (uint32_t)v; n->have_last = true; }
}

static void end_now(usagedata_t *d, ud_host_t *h)
{
    (void)d;
    h->now.used = true;
}

static void merge_now(const usagedata_t *d, ud_view_t *out)
{
    ud_now_view_t *n = &out->now;
    memset(n, 0, sizeof *n);
    int64_t best_when = INT64_MIN;
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->now.used) continue;
        const ud_now_t *s = &h->now;
        n->present = true;
        n->tokens += s->tokens;
        n->avg += s->avg;
        n->cost += s->cost;
        n->msgs += s->msgs;
        n->sessions += s->sessions;
        if (!s->have_last) continue;
        /* When that last message happened, on the board's clock. */
        int64_t when = h->now_sent_us - (int64_t)s->last_secs * 1000000LL;
        if (when > best_when) {
            best_when = when;
            n->have_last = true;
            n->last_secs = s->last_secs;
            n->last_sent_us = h->now_sent_us;
            n->session_secs = s->session_secs;
            strncpy(n->model, s->model, UD_NAME_MAX);
            strncpy(n->project, s->project, UD_NAME_MAX);
        }
    }
}

static bool used_now(const ud_host_t *h)
{
    return h->now.used;
}

const ud_section_t ud_section_now = {
    "!now", UD_NOW, true,
    begin_now, line_now, end_now, merge_now, used_now
};
