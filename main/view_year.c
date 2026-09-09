#include "view_common.h"

#include <stdio.h>
#include <string.h>

void views_year(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    (void)t;
    canvas_clear(c);
    char fresh[24];
    vw_freshness(d, now_us, fresh, sizeof fresh);
    vw_title(c, "LAST 12 MONTHS", fresh);

    const ud_year_view_t *y = &d->year;
    if (!y->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    /* The Mac starts the grid on a Sunday, so cell i is week i/7, row i%7.
       Weekday labels sit in a gutter to the left, months above. */
    const int grid_row = 2;
    const int x0 = 4 * c->cell_w + 4;
    const int y0 = grid_row * c->cell_h;
    int weeks = (y->len + 6) / 7;

    for (int w = 0; w < weeks; w++) {
        int yy, mm, dd;
        timecalc_civil(y->start + 7 * w, &yy, &mm, &dd);
        /* The week that contains the first of a month carries its name. */
        int x = x0 + w * HEAT_PITCH_X;
        if (dd <= 7 && x + 3 * c->cell_w <= c->w)
            canvas_puts_px(c, x, y0 - c->cell_h, timecalc_month_abbr(mm), PAL_DIM);
    }
    canvas_puts_px(c, 6, y0 + 1 * c->cell_h, "Mon", PAL_DIM);
    canvas_puts_px(c, 6, y0 + 3 * c->cell_h, "Wed", PAL_DIM);
    canvas_puts_px(c, 6, y0 + 5 * c->cell_h, "Fri", PAL_DIM);

    for (int i = 0; i < y->len; i++)
        vw_heat_cell(c, x0 + (i / 7) * HEAT_PITCH_X, y0 + (i % 7) * c->cell_h,
                  y->level[i]);

    int row = grid_row + 7;
    canvas_puts_px(c, x0, row * c->cell_h, "Less", PAL_DIM);
    int lx = x0 + 5 * c->cell_w;
    for (int l = 1; l <= PAL_HEAT_STEPS; l++, lx += HEAT_PITCH_X)
        vw_heat_cell(c, lx, row * c->cell_h, l);
    canvas_puts_px(c, lx + c->cell_w, row * c->cell_h, "More", PAL_DIM);

    /* The figures, two columns like the original. */
    const int left = 1, right = c->cols / 2 + 1;
    const uint16_t hi = pal_heat(PAL_HEAT_STEPS);
    char buf[32];
    uint64_t total = y->tok[0] + y->tok[1] + y->tok[2] + y->tok[3];

    row += 2;
    vw_labelled(c, left, row, "Favorite model: ", y->fav[0] ? y->fav : "-", hi);
    vw_human(total, buf, sizeof buf);
    vw_labelled(c, right, row, "Total tokens: ", buf, hi);

    row += 2;
    snprintf(buf, sizeof buf, "%lu", (unsigned long)y->sessions);
    vw_labelled(c, left, row, "Sessions: ", buf, hi);
    vw_duration(y->longest_secs, buf, sizeof buf);
    vw_labelled(c, right, row, "Longest session: ", buf, hi);

    row++;
    snprintf(buf, sizeof buf, "%d/%d", y->active_days, y->span_days);
    vw_labelled(c, left, row, "Active days: ", buf, hi);
    snprintf(buf, sizeof buf, "%d day%s", y->longest_streak,
             y->longest_streak == 1 ? "" : "s");
    vw_labelled(c, right, row, "Longest streak: ", buf, hi);

    row++;
    if (y->peak_index >= 0) vw_day_name(y->start + y->peak_index, buf, sizeof buf);
    else snprintf(buf, sizeof buf, "-");
    vw_labelled(c, left, row, "Most active day: ", buf, hi);
    snprintf(buf, sizeof buf, "%d day%s", y->current_streak,
             y->current_streak == 1 ? "" : "s");
    vw_labelled(c, right, row, "Current streak: ", buf, hi);

    row++;
    char in[16], out[16], cr[16], cw[16], line[128];
    vw_human(y->tok[0], in, sizeof in);
    vw_human(y->tok[1], out, sizeof out);
    vw_human(y->tok[2], cr, sizeof cr);
    vw_human(y->tok[3], cw, sizeof cw);
    snprintf(line, sizeof line, "In %s . Out %s . Cache %s read . %s write",
             in, out, cr, cw);
    vw_footer_fit(c, line);
    canvas_puts(c, left, row, line, PAL_DIM);

    if (y->estimated > 0) {
        snprintf(line, sizeof line,
                 "%d older day%s estimated from message counts; the rest measured",
                 y->estimated, y->estimated == 1 ? "" : "s");
        vw_footer_fit(c, line);
        canvas_puts(c, left, row + 1, line, PAL_DIM);
    }

    snprintf(line, sizeof line,
             "cell = a day's tokens, shaded by quartile   figures = all time");
    vw_footer(c, line);
}
