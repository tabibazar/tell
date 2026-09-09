#include "view_common.h"

#include <stdio.h>
#include <string.h>

#define MODEL_LINES 3     /* series drawn; more would be unreadable */
#define MODEL_CARDS 4

void views_models(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    char fresh[24];
    vw_freshness(d, now_us, fresh, sizeof fresh);
    vw_title(c, "TOKENS PER DAY BY MODEL", fresh);

    if (d->model_count == 0) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    int lines = d->model_count < MODEL_LINES ? d->model_count : MODEL_LINES;

    /* Plot area: a gutter for the value axis, rows 1..8 tall. */
    const int date_row = 9, legend_row = 10, card_row = 11;
    int gutter = 7 * c->cell_w;
    int top = c->cell_h + c->cell_h / 2;
    int bottom = date_row * c->cell_h - 4;
    int plot_h = bottom - top;
    int plot_w = c->w - gutter - 2 * c->cell_w;

    uint64_t peak = 1;
    for (int s = 0; s < lines; s++)
        for (int i = 0; i < d->mlen; i++)
            if (d->model_day[s][i] > peak) peak = d->model_day[s][i];

    for (int q = 0; q <= 4; q++) {
        int yy = bottom - plot_h * q / 4;
        canvas_fill_rect(c, gutter, yy, plot_w, 1, PAL_DIM);
        char lab[16];
        vw_human(peak * (uint64_t)q / 4, lab, sizeof lab);
        vw_right_text(c, yy / c->cell_h, gutter / c->cell_w - 1, lab, PAL_DIM);
    }
    canvas_fill_rect(c, gutter, top, 1, plot_h + 1, PAL_DIM);

    if (d->mlen == 0) {
        canvas_puts(c, gutter / c->cell_w + 2, 5,
                    "no daily data -- update tools/claude-stats.py", PAL_DIM);
    }

    /* Step lines, biggest model drawn last so it sits on top. Each day is a
       flat run at its value and a riser to the next; two pixels thick. */
    for (int s = lines - 1; s >= 0; s--) {
        uint16_t colour = pal_accent(s);
        int prev_y = -1;
        for (int i = 0; i < d->mlen; i++) {
            int x0 = gutter + plot_w * i / d->mlen;
            int x1 = gutter + plot_w * (i + 1) / d->mlen;
            int h = (int)((double)d->model_day[s][i] / (double)peak * plot_h * t);
            int yy = bottom - h;
            if (prev_y >= 0 && prev_y != yy) {
                int lo = prev_y < yy ? prev_y : yy;
                canvas_fill_rect(c, x0, lo, 2, (prev_y > yy ? prev_y : yy) - lo + 2,
                                 colour);
            }
            canvas_fill_rect(c, x0, yy, x1 - x0, 2, colour);
            prev_y = yy;
        }
    }

    /* Dates along the bottom, a fortnight apart, from the first day. */
    if (d->mlen > 0) {
        for (int i = 0; i < d->mlen; i += 14) {
            char lab[16];
            vw_day_name(d->mstart + i, lab, sizeof lab);
            int col = (gutter + plot_w * i / d->mlen) / c->cell_w;
            if (col + (int)strlen(lab) <= c->cols)
                canvas_puts(c, col, date_row, lab, PAL_DIM);
        }
    }

    /* Legend: which colour is which model. */
    int col = 1;
    for (int s = 0; s < lines; s++) {
        if (s > 0) { canvas_puts(c, col, legend_row, ".", PAL_DIM); col += 2; }
        vw_dot(c, col, legend_row, pal_accent(s));
        canvas_puts(c, col + 1, legend_row, d->models[s].name, PAL_FG);
        col += 1 + (int)strlen(d->models[s].name) + 1;
    }

    /* Cards: the biggest models with their all-time figures. Two lines each
       across the full width; at 64 columns two side by side do not fit. */
    uint64_t grand = 0;
    for (int i = 0; i < d->model_count; i++)
        grand += d->models[i].in + d->models[i].out + d->models[i].cread
               + d->models[i].cwrite;
    if (grand == 0) grand = 1;

    int cards = d->model_count < MODEL_CARDS ? d->model_count : MODEL_CARDS;
    for (int i = 0; i < cards; i++) {
        const ud_model_t *m = &d->models[i];
        int row = card_row + i * 2;
        uint64_t mine = m->in + m->out + m->cread + m->cwrite;
        char buf[96], a[16], b[16], cr[16], cw[16];

        vw_dot(c, 1, row, pal_accent(i));
        canvas_puts(c, 2, row, m->name, PAL_FG);
        snprintf(buf, sizeof buf, " (%.1f%%)", 100.0 * (double)mine / (double)grand);
        canvas_puts(c, 2 + (int)strlen(m->name), row, buf, PAL_DIM);

        vw_human(m->in, a, sizeof a);
        vw_human(m->out, b, sizeof b);
        vw_human(m->cread, cr, sizeof cr);
        vw_human(m->cwrite, cw, sizeof cw);
        snprintf(buf, sizeof buf, "In %s . Out %s . Cache %s read . %s write",
                 a, b, cr, cw);
        vw_footer_fit(c, buf);
        canvas_puts(c, 3, row + 1, buf, PAL_DIM);
    }

    char note[128];
    snprintf(note, sizeof note,
             "line = in+out+cache per day, last %d days   cards = all time",
             d->mlen > 0 ? d->mlen : UD_MDAYS);
    vw_footer(c, note);
}
