#include "ud_internal.h"

#include <stdlib.h>
#include <string.h>

/*
 * The account's rate limits, as Claude Code's own /usage command reports
 * them: the five-hour session window, the weekly allowance, and whichever
 * per-model weekly limit is in force.
 *
 * These belong to the account, not to the machine that sent them, so the
 * merge takes the freshest report outright rather than summing: two Macs
 * pushing would be two views of the same number, and adding them would say
 * the account was twice as used as it is.
 *
 * A reset arrives as the seconds remaining when the payload was written, not
 * as a wall-clock time, so the board needs no calendar to count it down: it
 * subtracts however long the payload has been sitting there. That also means
 * a stale payload reads as a countdown that has run out, which is honest --
 * the board cannot know what happened after the last push.
 */
static void begin_limits(usagedata_t *d, ud_host_t *h, int64_t now_us)
{
    (void)d; (void)now_us;
    memset(&h->limits, 0, sizeof h->limits);
}

static void line_limits(usagedata_t *d, ud_host_t *h, const char *tag, const char *q)
{
    (void)d;
    ud_limits_t *l = &h->limits;
    char num[24];

    if (strcmp(tag, "lim") == 0) {
        if (l->count >= UD_LIMIT_MAX) return;
        ud_limit_t *row = &l->rows[l->count];
        memset(row, 0, sizeof *row);

        q = ud_token(q, row->kind, UD_LIMIT_KIND);
        if (q == NULL) return;
        q = ud_token(q, num, sizeof num - 1);
        if (q == NULL) return;
        long pct = strtol(num, NULL, 10);
        row->percent = (uint8_t)(pct < 0 ? 0 : (pct > 100 ? 100 : pct));
        q = ud_token(q, num, sizeof num - 1);
        if (q == NULL) return;
        row->reset_secs = (uint32_t)strtoul(num, NULL, 10);
        q = ud_token(q, num, sizeof num - 1);
        if (q == NULL) return;
        row->active = num[0] == '1';
        q = ud_token(q, num, sizeof num - 1);
        if (q == NULL) return;
        row->severity = (uint8_t)strtol(num, NULL, 10);
        /* The scope is one token and optional: "-" where the limit covers
           everything rather than a named model. */
        if (ud_token(q, row->scope, UD_LIMIT_SCOPE) != NULL
         && strcmp(row->scope, "-") == 0)
            row->scope[0] = '\0';
        l->count++;
        l->used = true;
    } else if (strcmp(tag, "credits") == 0) {
        q = ud_token(q, num, sizeof num - 1);
        if (q == NULL) return;
        l->credit_used = (uint32_t)strtoul(num, NULL, 10);
        q = ud_token(q, num, sizeof num - 1);
        if (q == NULL) return;
        l->credit_limit = (uint32_t)strtoul(num, NULL, 10);
        q = ud_token(q, num, sizeof num - 1);
        if (q == NULL) return;
        long pct = strtol(num, NULL, 10);
        l->credit_pct = (uint8_t)(pct < 0 ? 0 : (pct > 100 ? 100 : pct));
        ud_token(q, l->currency, UD_LIMIT_CCY);
        l->have_credits = true;
        l->used = true;
    }
}

static void merge_limits(const usagedata_t *d, ud_view_t *out)
{
    ud_limits_view_t *v = &out->limits;
    memset(v, 0, sizeof *v);

    const ud_host_t *best = NULL;
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->limits.used) continue;
        if (best == NULL || h->updated_us > best->updated_us) best = h;
    }
    if (best == NULL) return;

    v->present = true;
    v->count = best->limits.count;
    memcpy(v->rows, best->limits.rows, sizeof v->rows);
    v->have_credits = best->limits.have_credits;
    v->credit_used = best->limits.credit_used;
    v->credit_limit = best->limits.credit_limit;
    v->credit_pct = best->limits.credit_pct;
    memcpy(v->currency, best->limits.currency, sizeof v->currency);
    /* When the countdowns were true. Everything on the page is measured
       from here. */
    v->sent_us = best->updated_us;
}

static bool used_limits(const ud_host_t *h)
{
    return h->limits.used;
}

const ud_section_t ud_section_limits = {
    "!limits", UD_LIMITS, true,
    begin_limits, line_limits, NULL, merge_limits, used_limits
};
