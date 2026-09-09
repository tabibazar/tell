#include "view_common.h"

#include <stdio.h>
#include <string.h>

void views_projects(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    const ud_projects_view_t *p = &d->projects;
    char head[48], fresh[24];
    vw_freshness(d, now_us, fresh, sizeof fresh);
    if (p->present) snprintf(head, sizeof head, "%d days   %s", p->days, fresh);
    else snprintf(head, sizeof head, "%s", fresh);
    vw_title(c, "BY PROJECT", head);
    if (!p->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    canvas_puts(c, 1, 1, "project", PAL_DIM);
    canvas_puts(c, 17, 1, "share of tokens", PAL_DIM);
    vw_right_text(c, 1, 52, "tokens", PAL_DIM);
    vw_right_text(c, 1, c->cols - 1, "cost", PAL_DIM);

    int bar_x = 17 * c->cell_w;
    int bar_max = (44 - 17) * c->cell_w;
    uint64_t peak = 1;
    for (int i = 0; i < p->count; i++)
        if (p->rows[i].tokens > peak) peak = p->rows[i].tokens;

    uint32_t sessions = 0, msgs = 0;
    uint64_t cents = 0;
    for (int i = 0; i < p->count && 2 + i < c->rows - 4; i++) {
        const ud_project_t *r = &p->rows[i];
        int row = 2 + i;
        canvas_puts(c, 1, row, r->name, PAL_FG);
        int width = (int)((double)r->tokens / (double)peak * bar_max * t);
        if (width < 2 && r->tokens > 0 && t >= 1.0f) width = 2;
        canvas_fill_rect(c, bar_x, row * c->cell_h + c->cell_h / 4, width,
                         c->cell_h / 2, pal_accent(i));
        char buf[24];
        vw_human(r->tokens, buf, sizeof buf);
        vw_right_text(c, row, 52, buf, PAL_FG);
        vw_money(r->cents, buf, sizeof buf);
        vw_right_text(c, row, c->cols - 1, buf, PAL_FG);
        sessions += r->sessions;
        msgs += r->msgs;
        cents += r->cents;
    }

    /* Shares, as text, under the table: the bars are relative to the biggest
       project, which is the readable choice, but the share is the number. */
    int row = 2 + p->count + 1;
    if (row < c->rows - 3 && p->total_tokens > 0) {
        char line[128] = "";
        int used = 0;
        for (int i = 0; i < p->count && i < 4; i++) {
            int pct = (int)((p->rows[i].tokens * 100 + p->total_tokens / 2) / p->total_tokens);
            used += snprintf(line + used, sizeof line - (size_t)used, "%s%s %d%%",
                             i ? "  .  " : "", p->rows[i].name, pct);
            if (used >= (int)sizeof line - 1) break;
        }
        vw_footer_fit(c, line);
        canvas_puts(c, 1, row, line, PAL_DIM);
    }

    char note[128], tot[24], cost[24];
    vw_human(p->total_tokens, tot, sizeof tot);
    vw_money(cents, cost, sizeof cost);
    snprintf(note, sizeof note, "%d projects   %lu sessions   %lu messages   %s   %s",
             p->count, (unsigned long)sessions, (unsigned long)msgs, tot, cost);
    vw_footer_fit(c, note);
    canvas_puts(c, 1, c->rows - 2, note, PAL_DIM);
    snprintf(note, sizeof note,
             "bar = in+out+cache tokens, from this Mac's transcripts only");
    vw_footer(c, note);
}
