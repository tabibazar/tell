#include "view_common.h"

#include <stdio.h>
#include <string.h>

void views_cost(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    char fresh[24];
    vw_freshness(d, now_us, fresh, sizeof fresh);
    vw_title(c, "IF THIS WERE THE API", fresh);

    const ud_cost_view_t *k = &d->cost;
    if (!k->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    const int left = 1, right = c->cols / 2 + 1;
    const uint16_t hi = pal_heat(PAL_HEAT_STEPS);
    char buf[32];

    vw_money(k->total, buf, sizeof buf);
    vw_labelled(c, left, 2, "All time: ", buf, hi);
    vw_money(k->last30, buf, sizeof buf);
    vw_labelled(c, right, 2, "Last 30 days: ", buf, hi);
    vw_money(k->last30 / 30, buf, sizeof buf);
    vw_labelled(c, left, 3, "Per day, last 30: ", buf, hi);
    vw_money(k->last7, buf, sizeof buf);
    vw_labelled(c, right, 3, "Last 7 days: ", buf, hi);
    if (k->plan > 0) {
        char plan[24];
        vw_money(k->plan, plan, sizeof plan);
        snprintf(buf, sizeof buf, "%s/mo", plan);
        vw_labelled(c, left, 4, "Plan: ", buf, PAL_FG);
        snprintf(buf, sizeof buf, "%.1fx", (double)k->last30 / (double)k->plan);
        vw_labelled(c, right, 4, "Last 30 days vs plan: ", buf, PAL_FG);
    }

    /* A bar per model, as on the tokens page, so the two read alike. */
    canvas_puts(c, left, 6, "model", PAL_DIM);
    canvas_puts(c, 17, 6, "share of all time", PAL_DIM);
    vw_right_text(c, 6, c->cols - 1, "cost", PAL_DIM);
    int bar_x = 17 * c->cell_w;
    int bar_max = c->w - bar_x - 10 * c->cell_w;
    uint64_t peak = 1;
    for (int i = 0; i < k->model_count; i++)
        if (k->models[i].cents > peak) peak = k->models[i].cents;
    int shown = k->model_count < 6 ? k->model_count : 6;
    for (int i = 0; i < shown; i++) {
        int row = 7 + i;
        canvas_puts(c, left, row, k->models[i].name, PAL_FG);
        int width = (int)((double)k->models[i].cents / (double)peak * bar_max * t);
        if (width < 2 && k->models[i].cents > 0 && t >= 1.0f) width = 2;
        canvas_fill_rect(c, bar_x, row * c->cell_h + c->cell_h / 4, width,
                         c->cell_h / 2, pal_accent(i));
        vw_money(k->models[i].cents, buf, sizeof buf);
        vw_right_text(c, row, c->cols - 1, buf, PAL_FG);
    }

    /* A bar per day. Thousandths of a dollar in, so /10 is cents. */
    const int head_row = 14, plot_top_row = 15, plot_bottom_row = 18, date_row = 18;
    uint64_t day_peak = 1, day_sum = 0;
    int peak_i = -1;
    for (int i = 0; i < k->len; i++) {
        day_sum += k->day[i];
        if (k->day[i] > day_peak) { day_peak = k->day[i]; peak_i = i; }
    }
    snprintf(buf, sizeof buf, "per day, last %d days", k->len);
    canvas_puts(c, left, head_row, buf, PAL_DIM);
    if (peak_i >= 0) {
        char m[24], dn[16], note[48];
        vw_money(day_peak / 10, m, sizeof m);
        vw_day_name(k->start + peak_i, dn, sizeof dn);
        snprintf(note, sizeof note, "peak %s on %s", m, dn);
        vw_right_text(c, head_row, c->cols - 1, note, PAL_PEAK);
    }
    int px0 = c->cell_w, plot_w = c->w - 2 * c->cell_w;
    int top = plot_top_row * c->cell_h;
    int bottom = plot_bottom_row * c->cell_h - 2;
    int plot_h = bottom - top;
    canvas_fill_rect(c, px0, bottom, plot_w, 1, PAL_DIM);
    if (k->len > 0) {
        int slot = plot_w / k->len;
        int bar_w = slot > 3 ? slot - 2 : slot;
        for (int i = 0; i < k->len; i++) {
            int h = (int)((double)k->day[i] / (double)day_peak * plot_h * t);
            if (h < 1 && k->day[i] > 0 && t >= 1.0f) h = 1;
            uint16_t colour = i == peak_i ? PAL_PEAK : pal_heat(PAL_HEAT_STEPS - 1);
            canvas_fill_rect(c, px0 + i * slot + 1, bottom - h, bar_w, h, colour);
        }
        /* Dates a fortnight apart, as on the models page. */
        for (int i = 0; i < k->len; i += 14) {
            char lab[16];
            vw_day_name(k->start + i, lab, sizeof lab);
            int col = (px0 + i * slot) / c->cell_w;
            if (col + (int)strlen(lab) <= c->cols)
                canvas_puts(c, col, date_row, lab, PAL_DIM);
        }
    }
    (void)day_sum;

    char note[128];
    snprintf(note, sizeof note,
             "API list prices per Mtok; cache writes at 5-min rate unless 1h");
    vw_footer(c, note);
}
