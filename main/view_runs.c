#include "view_common.h"

#include <stdio.h>
#include <string.h>

void views_runs(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    const ud_runs_view_t *r = &d->runs;
    char head[72], fresh[24];
    vw_freshness(d, now_us, fresh, sizeof fresh);
    if (r->present) snprintf(head, sizeof head, "%d days   %s", r->days, fresh);
    else snprintf(head, sizeof head, "%s", fresh);
    vw_title(c, "WHAT CLAUDE RUNS", head);
    if (!r->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    canvas_puts(c, 1, 1, "program", PAL_DIM);
    canvas_puts(c, 17, 1, "share of commands", PAL_DIM);
    vw_right_text(c, 1, c->cols - 1, "commands", PAL_DIM);
    int bar_x = 17 * c->cell_w, bar_max = (46 - 17) * c->cell_w;
    uint32_t peak = 1;
    for (int i = 0; i < r->count; i++) if (r->rows[i].calls > peak) peak = r->rows[i].calls;
    int shown = r->count < 9 ? r->count : 9;
    for (int i = 0; i < shown; i++) {
        const ud_tool_t *row = &r->rows[i];
        int y = 2 + i;
        bool other = strcmp(row->name, "other") == 0;
        canvas_puts(c, 1, y, row->name, other ? PAL_DIM : PAL_FG);
        int width = (int)((double)row->calls / (double)peak * bar_max * t);
        if (width < 2 && row->calls > 0 && t >= 1.0f) width = 2;
        canvas_fill_rect(c, bar_x, y * c->cell_h + c->cell_h / 4, width, c->cell_h / 2,
                         other ? PAL_DIM : pal_accent(i));
        char buf[24];
        if (r->commands > 0) {
            snprintf(buf, sizeof buf, "%lu%%",
                     (unsigned long)(((uint64_t)row->calls * 100 + r->commands / 2) / r->commands));
            canvas_puts(c, 48, y, buf, PAL_DIM);
        }
        snprintf(buf, sizeof buf, "%lu", (unsigned long)row->calls);
        vw_right_text(c, y, c->cols - 1, buf, PAL_FG);
    }

    /* What kind of work: one bar split by category, with a legend. */
    char line[128];
    snprintf(line, sizeof line, "%lu Bash calls   %lu commands   %.1f per call",
             (unsigned long)r->calls, (unsigned long)r->commands,
             r->calls ? (double)r->commands / r->calls : 0.0);
    vw_footer_fit(c, line);
    canvas_puts(c, 1, 12, line, PAL_DIM);

    canvas_puts(c, 1, 13, "what kind of work", PAL_DIM);
    uint64_t total = 0;
    for (int k = 0; k < UD_RUN_CATS; k++) total += r->cats[k];
    int bx = c->cell_w, bw = (c->cols - 2) * c->cell_w;
    int by = 14 * c->cell_h + 4, bh = c->cell_h - 8;
    canvas_fill_rect(c, bx, by, bw, bh, 0x2124);
    int drawn = 0;
    for (int k = 0; k < UD_RUN_CATS && total > 0; k++) {
        int seg = (int)((double)r->cats[k] / (double)total * bw * t);
        if (k == UD_RUN_CATS - 1 && t >= 1.0f) seg = (int)(bw * t) - drawn;
        if (seg <= 0) continue;
        canvas_fill_rect(c, bx + drawn, by, seg, bh, k == UD_RUN_CATS - 1 ? PAL_DIM : pal_accent(k));
        if (seg > 2) canvas_fill_rect(c, bx + drawn + seg - 2, by, 2, bh, PAL_BG);   /* a gap */
        drawn += seg;
    }
    /* Legend in two rows of four, each with its share. */
    for (int k = 0; k < UD_RUN_CATS; k++) {
        int row = 15 + k / 4, col = 1 + (k % 4) * 16;
        vw_dot(c, col, row, k == UD_RUN_CATS - 1 ? PAL_DIM : pal_accent(k));
        char buf[32];
        int pct = total ? (int)(((uint64_t)r->cats[k] * 100 + total / 2) / total) : 0;
        snprintf(buf, sizeof buf, "%s %d%%", usagedata_run_cats[k], pct);
        canvas_puts(c, col + 1, row, buf, PAL_FG);
    }

    snprintf(line, sizeof line,
             "each pipe stage and && step counts once, heredoc bodies not");
    vw_footer(c, line);
}
