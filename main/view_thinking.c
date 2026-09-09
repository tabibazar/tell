#include "view_common.h"

#include <stdio.h>
#include <string.h>

void views_thinking(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    const ud_thinking_view_t *th = &d->thinking;
    char head[48], fresh[24];
    vw_freshness(d, now_us, fresh, sizeof fresh);
    if (th->present) snprintf(head, sizeof head, "%d days   %s", th->days, fresh);
    else snprintf(head, sizeof head, "%s", fresh);
    vw_title(c, "THINKING SHARE", head);
    if (!th->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    const uint16_t hi = pal_heat(PAL_HEAT_STEPS);
    const int left = 1, right = c->cols / 2 + 1;
    char buf[80], a[24], b[24];
    snprintf(buf, sizeof buf, "%d%% of output tokens", th->share_pct);
    vw_labelled(c, left, 2, "Thinking: ", buf, hi);
    vw_money(th->think_cents, a, sizeof a);
    vw_labelled(c, right, 2, "Cost of thinking: ", a, hi);
    vw_human(th->thinking, a, sizeof a);
    vw_human(th->visible, b, sizeof b);
    snprintf(buf, sizeof buf, "%s thinking . %s visible", a, b);
    canvas_puts(c, left, 3, buf, PAL_DIM);
    vw_money(th->out_cents, a, sizeof a);
    snprintf(buf, sizeof buf, "of %s spent on output", a);
    canvas_puts(c, right, 3, buf, PAL_DIM);

    canvas_puts(c, left, 5, "model", PAL_DIM);
    canvas_puts(c, 17, 5, "share of output that is thinking", PAL_DIM);
    vw_right_text(c, 5, c->cols - 1, "thinking", PAL_DIM);
    int bar_x = 17 * c->cell_w, bar_max = (46 - 17) * c->cell_w;
    int shown = th->model_count < 6 ? th->model_count : 6;
    for (int i = 0; i < shown; i++) {
        const ud_think_model_t *m = &th->models[i];
        int row = 6 + i;
        uint64_t out = m->thinking + m->visible;
        int pct = out ? (int)((m->thinking * 100 + out / 2) / out) : 0;
        canvas_puts(c, left, row, m->name, PAL_FG);
        canvas_fill_rect(c, bar_x, row * c->cell_h + c->cell_h / 4, bar_max, c->cell_h / 2, 0x2124);
        canvas_fill_rect(c, bar_x, row * c->cell_h + c->cell_h / 4,
                         (int)(bar_max * pct / 100 * t), c->cell_h / 2, pal_accent(i));
        snprintf(buf, sizeof buf, "%d%%", pct);
        canvas_puts(c, 48, row, buf, PAL_FG);
        vw_human(m->thinking, a, sizeof a);
        vw_right_text(c, row, c->cols - 1, a, PAL_FG);
    }

    canvas_puts(c, left, 12, "per day, share of output that is thinking", PAL_DIM);
    int64_t vals[UD_MDAYS];
    int64_t top = 0;
    for (int i = 0; i < th->len; i++) {
        vals[i] = th->pct[i];
        if (vals[i] > top) top = vals[i];
    }
    /* Round the top up to a ten so the axis has headroom and round labels. */
    top = ((top + 9) / 10) * 10;
    if (top < 10) top = 10;
    if (top > 100) top = 100;
    vw_step_plot(c, vals, th->len, 0, top, th->start, 13, 18, hi, t, vw_label_pct);

    char note[128];
    snprintf(note, sizeof note,
             "share = thinking / output tokens, where the transcript reports the split");
    vw_footer(c, note);
}
