#include "view_common.h"

#include <stdio.h>
#include <string.h>

void views_stats(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    char head[48], fresh[24];
    vw_freshness(d, now_us, fresh, sizeof fresh);
    if (d->host_count > 1)
        snprintf(head, sizeof head, "%d machines   %s", d->host_count, fresh);
    else
        snprintf(head, sizeof head, "%s", fresh);
    vw_title(c, "USAGE BY MODEL", head);

    if (d->model_count == 0) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    uint64_t peak = 1, grand = 0;
    for (int i = 0; i < d->model_count; i++) {
        uint64_t t = d->models[i].cread + d->models[i].out;
        grand += t;
        if (t > peak) peak = t;
    }

    canvas_puts(c, 1, 1, "model", PAL_DIM);
    canvas_puts(c, 17, 1, "share of busiest model", PAL_DIM);
    vw_right_text(c, 1, c->cols - 1, "total", PAL_DIM);

    int bar_x = 17 * c->cell_w;
    int bar_max = c->w - bar_x - 10 * c->cell_w;

    /* Quarter gridlines behind the bars, so a bar's length has a value. */
    int first_row = 3, last_row = first_row + d->model_count;
    for (int q = 1; q <= 4; q++) {
        int x = bar_x + bar_max * q / 4;
        canvas_fill_rect(c, x, first_row * c->cell_h, 1,
                         (last_row - first_row) * c->cell_h, PAL_DIM);
        char lab[16];
        vw_human(peak * (uint64_t)q / 4, lab, sizeof lab);
        canvas_puts(c, x / c->cell_w - (int)strlen(lab) + 1, 2, lab, PAL_DIM);
    }

    for (int i = 0; i < d->model_count && first_row + i < c->rows - 2; i++) {
        int row = first_row + i;
        canvas_puts(c, 1, row, d->models[i].name, PAL_FG);

        uint64_t total = d->models[i].cread + d->models[i].out;
        int width = (int)((double)total / (double)peak * bar_max * t);
        if (width < 2 && total > 0 && t >= 1.0f) width = 2;
        canvas_fill_rect(c, bar_x, row * c->cell_h + c->cell_h / 4,
                         width, c->cell_h / 2, pal_accent(i));

        char value[16];
        vw_human(total, value, sizeof value);
        vw_right_text(c, row, c->cols - 1, value, PAL_FG);
    }

    char note[128], total[16];
    vw_human(grand, total, sizeof total);
    snprintf(note, sizeof note,
             "bar = output + cache-read tokens   %d models   %s total",
             d->model_count, total);
    vw_footer(c, note);
}

void views_daily(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);

    if (d->day_count == 0) {
        vw_title(c, "TOKENS PER DAY", NULL);
            canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    char range[64], fresh[24];
    vw_freshness(d, now_us, fresh, sizeof fresh);
    snprintf(range, sizeof range, "%s to %s   %s",
             d->days[0].label, d->days[d->day_count - 1].label, fresh);
    vw_title(c, "TOKENS PER DAY", range);

    uint64_t peak = 1, grand = 0;
    int peak_i = 0;
    for (int i = 0; i < d->day_count; i++) {
        grand += d->days[i].tokens;
        if (d->days[i].tokens > peak) { peak = d->days[i].tokens; peak_i = i; }
    }

    /* Left gutter for the value axis; bottom rows for dates, legend, notes. */
    int gutter = 7 * c->cell_w;
    int top = 2 * c->cell_h;
    int bottom = (c->rows - 4) * c->cell_h;
    int plot_h = bottom - top;
    int plot_w = c->w - gutter - c->cell_w;

    for (int q = 0; q <= 4; q++) {
        int y = bottom - plot_h * q / 4;
        canvas_fill_rect(c, gutter, y, plot_w, 1, PAL_DIM);
        char lab[16];
        vw_human(peak * (uint64_t)q / 4, lab, sizeof lab);
        vw_right_text(c, y / c->cell_h, gutter / c->cell_w - 1, lab, PAL_DIM);
    }
    canvas_fill_rect(c, gutter, top, 1, plot_h + 1, PAL_DIM);

    int slot = plot_w / d->day_count;
    int bar_w = slot > 4 ? slot - 4 : slot;
    int last = d->day_count - 1;
    uint64_t mean = grand / (uint64_t)d->day_count;

    for (int i = 0; i < d->day_count; i++) {
        int h = (int)((double)d->days[i].tokens / (double)peak * plot_h * t);
        if (h < 2 && d->days[i].tokens > 0 && t >= 1.0f) h = 2;

        /* One colour for an ordinary day: cycling accents implied categories
           that do not exist. The peak and the latest day are the only
           distinctions, and each is marked with a symbol too, so the meaning
           survives if the hues cannot be told apart. */
        uint16_t colour = PAL_A0;
        if (i == last) colour = PAL_LATEST;
        else if (i == peak_i) colour = PAL_PEAK;

        int bx = gutter + i * slot + 2;

        /* With one machine there is nothing to stack, so colour by weekday
           instead: the bar still carries information, and the chart is not a
           single hue. With several machines the split matters more. */
        if (d->host_count <= 1) {
            uint16_t wc = pal_weekday(d->days[i].dow);
            canvas_fill_rect(c, bx, bottom - h, bar_w, h, wc);
            if (h >= 3)
                canvas_fill_rect(c, bx, bottom - h, bar_w, 2, pal_lighten(wc));
            goto marks;
        }

        /* Stack the machines up the bar, newest colour on top. */
        int drawn = 0;
        uint64_t day_total = d->days[i].tokens;
        for (int hh = 0; hh < d->host_count && day_total > 0; hh++) {
            int seg = (int)((double)d->day_by_host[i][hh] / (double)day_total
                            * (double)h);
            if (hh == d->host_count - 1) seg = h - drawn;
            if (seg <= 0) continue;
            uint16_t hc = pal_accent(hh);
            canvas_fill_rect(c, bx, bottom - drawn - seg, bar_w, seg, hc);
            canvas_fill_rect(c, bx, bottom - drawn - seg, bar_w, 2,
                             pal_lighten(hc));
            drawn += seg;
        }
        if (d->host_count == 0)
            canvas_fill_rect(c, bx, bottom - h, bar_w, h, colour);

marks: ;   /* C11 needs a statement, not a declaration, after a label */

        int label_col = (gutter + i * slot) / c->cell_w;
        if (slot >= 6 * c->cell_w || (i % 2) == 0)
            canvas_puts(c, label_col, c->rows - 3, d->days[i].label, PAL_DIM);
        /* Redundant, non-colour marking of the notable bars. */
        if (i == peak_i)
            canvas_puts(c, label_col + 2, c->rows - 4, "^", PAL_PEAK);
        if (i == last)
            canvas_puts(c, label_col + 2, c->rows - 4, ">", PAL_LATEST);
    }

    /* A dashed average line, so a bar reads as above or below typical. */
    int mean_y = bottom - (int)((double)mean / (double)peak * plot_h);
    for (int x = gutter; x < gutter + plot_w; x += 12)
        canvas_fill_rect(c, x, mean_y, 6, 1, PAL_FG);
    canvas_puts(c, (gutter + plot_w) / c->cell_w - 3, mean_y / c->cell_h,
                "avg", PAL_FG);

    /* The peak is the number people look for, so put it on the bar. */
    {
        char plab[16];
        vw_human(peak, plab, sizeof plab);
        int col = (gutter + peak_i * slot) / c->cell_w - 1;
        if (col < 0) col = 0;
        canvas_puts(c, col, top / c->cell_h - 1, plab, PAL_PEAK);
    }

    /* Legend: swatches, so the three colours are not left to guess at. */
    int row = c->rows - 2;
    int y = row * c->cell_h + c->cell_h / 4;
    int sw = c->cell_w;
    canvas_fill_rect(c, 1 * c->cell_w, y, sw, c->cell_h / 2, PAL_A0);
    canvas_puts(c, 3, row, "a day", PAL_DIM);
    canvas_fill_rect(c, 10 * c->cell_w, y, sw, c->cell_h / 2, PAL_PEAK);
    canvas_puts(c, 12, row, "^ busiest", PAL_DIM);
    canvas_fill_rect(c, 24 * c->cell_w, y, sw, c->cell_h / 2, PAL_LATEST);
    canvas_puts(c, 26, row, "> most recent", PAL_DIM);

    char peak_lab[16], avg[16], total[16];
    vw_human(peak, peak_lab, sizeof peak_lab);
    vw_human(grand / (uint64_t)d->day_count, avg, sizeof avg);
    vw_human(grand, total, sizeof total);
    canvas_puts(c, 41, row, "bar = in+out+cache", PAL_DIM);

    char note[128];
    snprintf(note, sizeof note,
             "peak %s on %s   avg %s/day   %s over %d days",
             peak_lab, d->days[peak_i].label, avg, total, d->day_count);
    vw_footer(c, note);
}
