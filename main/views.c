#include "views.h"

#include "palette.h"

#include <stdio.h>

/* Compact magnitude: a billion has to fit in a narrow column. */
static void human(uint64_t n, char *out, int size)
{
    if (n >= 1000000000ULL) snprintf(out, size, "%.1fB", n / 1e9);
    else if (n >= 1000000ULL) snprintf(out, size, "%.1fM", n / 1e6);
    else if (n >= 1000ULL) snprintf(out, size, "%.1fK", n / 1e3);
    else snprintf(out, size, "%llu", (unsigned long long)n);
}

static void title(canvas_t *c, const char *text)
{
    canvas_fill_rect(c, 0, 0, c->w, c->cell_h, PAL_TITLE_BG);
    canvas_puts(c, 1, 0, text, PAL_FG);
}

void views_stats(canvas_t *c, const usagedata_t *d)
{
    canvas_clear(c);
    title(c, "USAGE BY MODEL");

    if (d->model_count == 0) {
        canvas_puts(c, 1, 2, "no data yet", PAL_DIM);
        return;
    }

    uint64_t peak = 1;
    for (int i = 0; i < d->model_count; i++) {
        uint64_t t = d->models[i].cread + d->models[i].out;
        if (t > peak) peak = t;
    }

    int cell_w = c->cell_w;
    int cell_h = c->cell_h;
    int bar_x = 17 * cell_w;
    int bar_max = c->w - bar_x - 9 * cell_w;

    for (int i = 0; i < d->model_count && i + 2 < c->rows; i++) {
        int row = i + 2;
        canvas_puts(c, 1, row, d->models[i].name, PAL_FG);

        uint64_t total = d->models[i].cread + d->models[i].out;
        int width = (int)((double)total / (double)peak * bar_max);
        if (width < 2 && total > 0) width = 2;
        canvas_fill_rect(c, bar_x, row * cell_h + cell_h / 4,
                         width, cell_h / 2, pal_accent(i));

        char value[16];
        human(total, value, sizeof value);
        canvas_puts(c, c->cols - 8, row, value, PAL_DIM);
    }
}

void views_daily(canvas_t *c, const usagedata_t *d)
{
    canvas_clear(c);
    title(c, "TOKENS PER DAY");

    if (d->day_count == 0) {
        canvas_puts(c, 1, 2, "no data yet", PAL_DIM);
        return;
    }

    uint64_t peak = 1;
    for (int i = 0; i < d->day_count; i++)
        if (d->days[i].tokens > peak) peak = d->days[i].tokens;

    int cell_w = c->cell_w;
    int cell_h = c->cell_h;
    int top = 2 * cell_h;
    int bottom = c->h - cell_h;          /* leave a row for date labels */
    int plot_h = bottom - top;
    int slot = c->w / d->day_count;
    int bar_w = slot > 4 ? slot - 4 : slot;

    for (int i = 0; i < d->day_count; i++) {
        int h = (int)((double)d->days[i].tokens / (double)peak * plot_h);
        if (h < 2 && d->days[i].tokens > 0) h = 2;
        canvas_fill_rect(c, i * slot + 2, bottom - h, bar_w, h, pal_accent(i));

        /* Labels are 5 chars; print every other one when the slots are tight. */
        if (slot >= 6 * cell_w || (i % 2) == 0)
            canvas_puts(c, (i * slot) / cell_w, c->rows - 1,
                        d->days[i].label, PAL_DIM);
    }

    char value[16];
    human(peak, value, sizeof value);
    canvas_puts(c, c->cols - 9, 1, value, PAL_DIM);
}
