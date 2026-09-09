#include "view_common.h"

#include <stdio.h>
#include <string.h>

void views_now(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    (void)t;
    canvas_clear(c);
    char fresh[24];
    vw_freshness(d, now_us, fresh, sizeof fresh);
    vw_title(c, "TODAY, LIVE", fresh);

    const ud_now_view_t *n = &d->now;
    if (!n->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    const uint16_t hi = pal_heat(PAL_HEAT_STEPS);
    const int left = 1, right = c->cols / 2 + 1;
    char buf[64], a[24];

    /* The status line first: it is what a glance is for. */
    if (n->have_last) {
        int64_t since = n->last_secs + (now_us - n->last_sent_us) / 1000000;
        if (since < 0) since = 0;
        vw_age((uint32_t)since, a, sizeof a);
        if (since < UD_BUSY_SECS) {
            canvas_puts(c, left, 2, "Claude is busy", hi);
            snprintf(buf, sizeof buf, "   last message %s ago", a);
            canvas_puts(c, left + 14, 2, buf, PAL_DIM);
        } else {
            canvas_puts(c, left, 2, "Claude is idle", PAL_FG);
            snprintf(buf, sizeof buf, "   last message %s ago", a);
            canvas_puts(c, left + 14, 2, buf, PAL_DIM);
        }
        snprintf(buf, sizeof buf, "on %s with %s", n->project[0] ? n->project : "?",
                 n->model[0] ? n->model : "?");
        canvas_puts(c, left, 3, buf, PAL_DIM);
    } else {
        canvas_puts(c, left, 2, "Nothing recorded yet today", PAL_DIM);
    }

    int row = 5;
    vw_human(n->tokens, a, sizeof a);
    vw_labelled(c, left, row, "Tokens today: ", a, hi);
    vw_money(n->cost, a, sizeof a);
    vw_labelled(c, right, row, "Cost today: ", a, hi);
    row++;
    snprintf(a, sizeof a, "%lu", (unsigned long)n->msgs);
    vw_labelled(c, left, row, "Messages: ", a, hi);
    snprintf(a, sizeof a, "%lu", (unsigned long)n->sessions);
    vw_labelled(c, right, row, "Sessions: ", a, hi);
    row++;
    if (n->have_last) {
        vw_age(n->session_secs, a, sizeof a);
        vw_labelled(c, left, row, "Current session: ", a, hi);
    }
    vw_human(n->avg, a, sizeof a);
    vw_labelled(c, right, row, "Typical day: ", a, hi);

    /* Today against a typical day, as one bar with a mark for typical. */
    row += 2;
    canvas_puts(c, left, row, "today against a typical day", PAL_DIM);
    uint64_t scale = n->tokens > n->avg ? n->tokens : n->avg;
    if (scale == 0) scale = 1;
    int bx = left * c->cell_w, bw = (c->cols - 2) * c->cell_w;
    int by = (row + 1) * c->cell_h + 4, bh = 2 * c->cell_h - 8;
    canvas_fill_rect(c, bx, by, bw, bh, 0x2124);
    int today_w = (int)((double)n->tokens / (double)scale * bw);
    canvas_fill_rect(c, bx, by, today_w, bh, hi);
    if (n->avg > 0) {
        int ax = bx + (int)((double)n->avg / (double)scale * bw);
        if (ax >= bx + bw) ax = bx + bw - 2;
        canvas_fill_rect(c, ax, by - 4, 2, bh + 8, PAL_FG);
        int lx = ax - 3 * c->cell_w;             /* centred under the mark */
        if (lx + 7 * c->cell_w > bx + bw) lx = bx + bw - 7 * c->cell_w;
        if (lx < bx) lx = bx;
        canvas_puts_px(c, lx, by + bh + 4, "typical", PAL_FG);
    }
    if (n->avg > 0) {
        snprintf(buf, sizeof buf, "%d%% of a typical day so far",
                 (int)((n->tokens * 100 + n->avg / 2) / n->avg));
        canvas_puts(c, left, row + 4, buf, PAL_DIM);
    }

    char note[96];
    snprintf(note, sizeof note, "from the transcripts, refreshed every minute");
    vw_footer(c, note);
}
