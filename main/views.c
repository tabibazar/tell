#include "views.h"

#include "palette.h"

#include <stdio.h>
#include <string.h>

/* Compact magnitude: a billion has to fit in a narrow column. */
static void human(uint64_t n, char *out, int size)
{
    if (n >= 1000000000ULL) snprintf(out, size, "%.1fB", n / 1e9);
    else if (n >= 1000000ULL) snprintf(out, size, "%.1fM", n / 1e6);
    else if (n >= 1000ULL) snprintf(out, size, "%.0fK", n / 1e3);
    else snprintf(out, size, "%llu", (unsigned long long)n);
}

static void title(canvas_t *c, const char *left, const char *right)
{
    canvas_fill_rect(c, 0, 0, c->w, c->cell_h, PAL_TITLE_BG);
    canvas_puts(c, 1, 0, left, PAL_FG);
    if (right)
        canvas_puts(c, c->cols - 1 - (int)strlen(right), 0, right, PAL_FG);
}

/* A footnote explaining what was actually measured. Without it a bar chart is
   decoration: the reader cannot tell tokens from calls, or which window. */
static void footer(canvas_t *c, const char *text)
{
    canvas_puts(c, 1, c->rows - 1, text, PAL_DIM);
}

static void right_text(canvas_t *c, int row, int right_col, const char *s,
                       uint16_t colour)
{
    canvas_puts(c, right_col - (int)strlen(s), row, s, colour);
}

void views_stats(canvas_t *c, const usagedata_t *d)
{
    canvas_clear(c);
    title(c, "USAGE BY MODEL", "tokens");

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
    right_text(c, 1, c->cols - 1, "total", PAL_DIM);

    int bar_x = 17 * c->cell_w;
    int bar_max = c->w - bar_x - 10 * c->cell_w;

    /* Quarter gridlines behind the bars, so a bar's length has a value. */
    int first_row = 3, last_row = first_row + d->model_count;
    for (int q = 1; q <= 4; q++) {
        int x = bar_x + bar_max * q / 4;
        canvas_fill_rect(c, x, first_row * c->cell_h, 1,
                         (last_row - first_row) * c->cell_h, PAL_DIM);
        char lab[16];
        human(peak * (uint64_t)q / 4, lab, sizeof lab);
        canvas_puts(c, x / c->cell_w - (int)strlen(lab) + 1, 2, lab, PAL_DIM);
    }

    for (int i = 0; i < d->model_count && first_row + i < c->rows - 2; i++) {
        int row = first_row + i;
        canvas_puts(c, 1, row, d->models[i].name, PAL_FG);

        uint64_t total = d->models[i].cread + d->models[i].out;
        int width = (int)((double)total / (double)peak * bar_max);
        if (width < 2 && total > 0) width = 2;
        canvas_fill_rect(c, bar_x, row * c->cell_h + c->cell_h / 4,
                         width, c->cell_h / 2, pal_accent(i));

        char value[16];
        human(total, value, sizeof value);
        right_text(c, row, c->cols - 1, value, PAL_FG);
    }

    char note[96], total[16];
    human(grand, total, sizeof total);
    snprintf(note, sizeof note,
             "bar = output + cache-read tokens   %d models   %s total",
             d->model_count, total);
    footer(c, note);
}

void views_daily(canvas_t *c, const usagedata_t *d)
{
    canvas_clear(c);

    if (d->day_count == 0) {
        title(c, "TOKENS PER DAY", NULL);
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    char range[24];
    snprintf(range, sizeof range, "%s to %s",
             d->days[0].label, d->days[d->day_count - 1].label);
    title(c, "TOKENS PER DAY", range);

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
        human(peak * (uint64_t)q / 4, lab, sizeof lab);
        right_text(c, y / c->cell_h, gutter / c->cell_w - 1, lab, PAL_DIM);
    }
    canvas_fill_rect(c, gutter, top, 1, plot_h + 1, PAL_DIM);

    int slot = plot_w / d->day_count;
    int bar_w = slot > 4 ? slot - 4 : slot;
    int last = d->day_count - 1;

    for (int i = 0; i < d->day_count; i++) {
        int h = (int)((double)d->days[i].tokens / (double)peak * plot_h);
        if (h < 2 && d->days[i].tokens > 0) h = 2;
        /* One colour for every day: cycling accents implied categories that
           do not exist. Today and the peak are the only distinctions. */
        uint16_t colour = PAL_A0;
        if (i == last) colour = PAL_A3;
        else if (i == peak_i) colour = PAL_A2;
        canvas_fill_rect(c, gutter + i * slot + 2, bottom - h, bar_w, h, colour);

        if (slot >= 6 * c->cell_w || (i % 2) == 0)
            canvas_puts(c, (gutter + i * slot) / c->cell_w, c->rows - 3,
                        d->days[i].label, PAL_DIM);
    }

    /* Legend: swatches, so the three colours are not left to guess at. */
    int row = c->rows - 2;
    int y = row * c->cell_h + c->cell_h / 4;
    int sw = c->cell_w;
    canvas_fill_rect(c, 1 * c->cell_w, y, sw, c->cell_h / 2, PAL_A0);
    canvas_puts(c, 3, row, "a day", PAL_DIM);
    canvas_fill_rect(c, 10 * c->cell_w, y, sw, c->cell_h / 2, PAL_A2);
    canvas_puts(c, 12, row, "busiest", PAL_DIM);
    canvas_fill_rect(c, 22 * c->cell_w, y, sw, c->cell_h / 2, PAL_A3);
    canvas_puts(c, 24, row, "most recent", PAL_DIM);

    char peak_lab[16], avg[16], total[16];
    human(peak, peak_lab, sizeof peak_lab);
    human(grand / (uint64_t)d->day_count, avg, sizeof avg);
    human(grand, total, sizeof total);
    canvas_puts(c, 38, row, "bar = in+out+cache tokens", PAL_DIM);

    char note[96];
    snprintf(note, sizeof note,
             "peak %s on %s   avg %s/day   %s over %d days",
             peak_lab, d->days[peak_i].label, avg, total, d->day_count);
    footer(c, note);
}
