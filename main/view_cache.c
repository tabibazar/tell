#include "view_common.h"

#include <stdio.h>
#include <string.h>

void views_cache(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    char fresh[24];
    vw_freshness(d, now_us, fresh, sizeof fresh);
    vw_title(c, "CACHE EFFICIENCY", fresh);

    const ud_cache_view_t *k = &d->cache;
    if (!k->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    const uint16_t hi = pal_heat(PAL_HEAT_STEPS);
    const int left = 1, right = c->cols / 2 + 1;
    char buf[48], a[24];

    snprintf(buf, sizeof buf, "%d%%", k->hit_pct);
    vw_labelled(c, left, 2, "Input served from cache: ", buf, hi);
    vw_money(k->saved, a, sizeof a);
    vw_labelled(c, right, 2, "Saved by caching: ", a, hi);
    vw_money(k->saved + k->cost, a, sizeof a);
    vw_labelled(c, left, 3, "Without caching: ", a, hi);
    if (k->cost > 0) {
        snprintf(buf, sizeof buf, "%.1fx what it cost", (double)(k->saved + k->cost) / (double)k->cost);
        canvas_puts(c, right, 3, buf, PAL_DIM);
    }

    /* Per model: a bar on a fixed 0..100% scale, so bars are comparable. */
    canvas_puts(c, left, 5, "model", PAL_DIM);
    canvas_puts(c, 17, 5, "share of input from cache", PAL_DIM);
    vw_right_text(c, 5, c->cols - 1, "saved", PAL_DIM);
    int bar_x = 17 * c->cell_w, bar_max = (45 - 17) * c->cell_w;
    int shown = k->model_count < 6 ? k->model_count : 6;
    for (int i = 0; i < shown; i++) {
        const ud_cache_model_t *m = &k->models[i];
        int row = 6 + i;
        uint64_t total = m->in + m->cread + m->cwrite;
        int pct = total ? (int)((m->cread * 100 + total / 2) / total) : 0;
        canvas_puts(c, left, row, m->name, PAL_FG);
        canvas_fill_rect(c, bar_x, row * c->cell_h + c->cell_h / 4, bar_max, c->cell_h / 2, 0x2124);
        canvas_fill_rect(c, bar_x, row * c->cell_h + c->cell_h / 4,
                         (int)(bar_max * pct / 100 * t), c->cell_h / 2, pal_accent(i));
        snprintf(buf, sizeof buf, "%d%%", pct);
        canvas_puts(c, 46, row, buf, PAL_FG);
        vw_money(m->saved_cents, a, sizeof a);
        vw_right_text(c, row, c->cols - 1, a, PAL_FG);
    }

    /* Per day: a step line on a 0..100% scale. */
    const int head_row = 12, plot_top_row = 13, date_row = 18;
    canvas_puts(c, left, head_row, "per day, share of input from cache", PAL_DIM);
    int gutter = 5 * c->cell_w;
    int top = plot_top_row * c->cell_h + 4;
    int bottom = date_row * c->cell_h - 4;
    int plot_h = bottom - top, plot_w = c->w - gutter - 2 * c->cell_w;

    /* The axis starts near the worst day rather than at zero: hit rates sit
       in the nineties, and against 0..100 every day would be one flat line.
       Rounded down to a ten, and never above 90 so there is always a range. */
    int lo = 100;
    for (int i = 0; i < k->len; i++)
        if (k->pct[i] >= 0 && k->pct[i] < lo) lo = k->pct[i];
    lo = (lo / 10) * 10;
    if (lo > 90) lo = 90;
    int span = 100 - lo;
    for (int q = 0; q <= 2; q++) {
        int yy = bottom - plot_h * q / 2;
        canvas_fill_rect(c, gutter, yy, plot_w, 1, PAL_DIM);
        snprintf(buf, sizeof buf, "%d%%", lo + span * q / 2);
        vw_right_text(c, yy / c->cell_h, gutter / c->cell_w - 1, buf, PAL_DIM);
    }
    if (k->len > 0) {
        int prev_y = -1;
        for (int i = 0; i < k->len; i++) {
            int x0 = gutter + plot_w * i / k->len;
            int x1 = gutter + plot_w * (i + 1) / k->len;
            if (k->pct[i] < 0) { prev_y = -1; continue; }
            int above = k->pct[i] - lo;
            if (above < 0) above = 0;
            int yy = bottom - (int)(plot_h * above / span * t);
            if (prev_y >= 0 && prev_y != yy) {
                int lo = prev_y < yy ? prev_y : yy;
                canvas_fill_rect(c, x0, lo, 2, (prev_y > yy ? prev_y : yy) - lo + 2, hi);
            }
            canvas_fill_rect(c, x0, yy, x1 - x0, 2, hi);
            prev_y = yy;
        }
        for (int i = 0; i < k->len; i += 14) {
            char lab[16];
            vw_day_name(k->start + i, lab, sizeof lab);
            int col = (gutter + plot_w * i / k->len) / c->cell_w;
            if (col + (int)strlen(lab) <= c->cols) canvas_puts(c, col, date_row, lab, PAL_DIM);
        }
    } else {
        canvas_puts(c, gutter / c->cell_w + 2, 15, "no daily data", PAL_DIM);
    }

    char note[128];
    snprintf(note, sizeof note,
             "saved = cached tokens at input price minus at cache-read price");
    vw_footer(c, note);
}
