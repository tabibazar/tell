#include "view_common.h"

#include <stdio.h>
#include <string.h>

void views_week(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    const ud_week_view_t *w = &d->week;
    char head[72], fresh[24];
    vw_freshness(d, now_us, fresh, sizeof fresh);
    if (w->present && w->today > 0) {
        char dn[16];
        vw_day_name(w->today, dn, sizeof dn);
        snprintf(head, sizeof head, "to %s   %s", dn, fresh);
    } else {
        snprintf(head, sizeof head, "%s", fresh);
    }
    vw_title(c, "THIS WEEK VS LAST", head);
    if (!w->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    static const char *const labels[UD_WEEK_ROWS] = {
        "Tokens", "Cost", "Messages", "Sessions", "Tool calls", "Active days"
    };
    const int col_this = 30, col_last = 46, col_change = c->cols - 1;
    vw_right_text(c, 2, col_this, "last 7 days", PAL_DIM);
    vw_right_text(c, 2, col_last, "the 7 before", PAL_DIM);
    vw_right_text(c, 2, col_change, "change", PAL_DIM);
    for (int r = 0; r < UD_WEEK_ROWS; r++) {
        int row = 3 + r;
        char a[24], b[24], ch[16];
        uint64_t now_v = w->this_week[r], then_v = w->last_week[r];
        if (r == 0) { vw_human(now_v, a, sizeof a); vw_human(then_v, b, sizeof b); }
        else if (r == 1) { vw_money(now_v, a, sizeof a); vw_money(then_v, b, sizeof b); }
        else {
            snprintf(a, sizeof a, "%llu", (unsigned long long)now_v);
            snprintf(b, sizeof b, "%llu", (unsigned long long)then_v);
        }
        canvas_puts(c, 1, row, labels[r], PAL_DIM);
        vw_right_text(c, row, col_this, a, PAL_FG);
        vw_right_text(c, row, col_last, b, PAL_DIM);
        /* Up is amber, down is blue, so the direction reads at a glance and
           the sign is there too for anyone who cannot tell them apart. */
        uint16_t colour = PAL_DIM;
        if (then_v == 0 && now_v == 0) snprintf(ch, sizeof ch, "-");
        else if (then_v == 0) { snprintf(ch, sizeof ch, "new"); colour = pal_heat(PAL_HEAT_STEPS); }
        else {
            long long pct = (long long)(((double)now_v - (double)then_v) / (double)then_v * 100.0);
            snprintf(ch, sizeof ch, "%s%lld%%", pct > 0 ? "+" : "", pct);
            if (pct > 0) colour = pal_heat(PAL_HEAT_STEPS);
            else if (pct < 0) colour = PAL_A0;
        }
        vw_right_text(c, row, col_change, ch, colour);
    }

    /* Two rows of daily bars, the earlier week in grey, so the shape of the
       week is visible as well as the totals. */
    uint64_t peak = 1;
    for (int i = 0; i < 14; i++) if (w->day[i] > peak) peak = w->day[i];
    const int top_row = 11, bottom_row = 17;
    int top = top_row * c->cell_h, bottom = bottom_row * c->cell_h - 2;
    int plot_h = bottom - top;
    for (int half = 0; half < 2; half++) {
        int x0 = (half == 0 ? 2 : 34) * c->cell_w;
        int width = 28 * c->cell_w, slot = width / 7, bar_w = slot - 8;
        canvas_puts(c, x0 / c->cell_w, top_row - 1,
                    half == 0 ? "the 7 before" : "last 7 days", PAL_DIM);
        canvas_fill_rect(c, x0, bottom, width, 1, PAL_DIM);
        for (int i = 0; i < 7; i++) {
            int idx = half * 7 + i;
            int h = (int)((double)w->day[idx] / (double)peak * plot_h * t);
            if (h < 1 && w->day[idx] > 0 && t >= 1.0f) h = 1;
            int bx = x0 + i * slot + 4;
            canvas_fill_rect(c, bx, bottom - h, bar_w, h,
                             half == 0 ? PAL_DIM : pal_heat(PAL_HEAT_STEPS));
            if (w->today > 0) {
                static const char *const wd = "SMTWTFS";
                char lab[2] = { wd[timecalc_weekday(w->today - 13 + idx)], '\0' };
                canvas_puts_px(c, bx + bar_w / 2 - c->cell_w / 2, bottom_row * c->cell_h, lab, PAL_DIM);
            }
        }
    }

    char note[96];
    snprintf(note, sizeof note,
             "bars = tokens per day; older days from Claude Code's own cache");
    vw_footer(c, note);
}
