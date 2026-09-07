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

/* Width of the machine-filter button, in character cells. */
#define BUTTON_COLS 12

bool views_button_hit(canvas_t *c, int x, int y)
{
    return y < c->cell_h && x > c->w - BUTTON_COLS * c->cell_w;
}

/* Draws the button showing which machine's tokens are on screen. It is drawn
   as a raised block with a border so it reads as pressable, rather than as a
   label someone has to guess is interactive. */
static void button(canvas_t *c, const ud_view_t *d)
{
    int bw = BUTTON_COLS * c->cell_w;
    int bx = c->w - bw;

    canvas_fill_rect(c, bx, 0, bw, c->cell_h, PAL_A1);
    canvas_fill_rect(c, bx, 0, bw, 2, pal_lighten(PAL_A1));

    const char *label = d->host[0] ? d->host : "ALL MACS";
    int len = (int)strlen(label);
    if (len > BUTTON_COLS - 2) len = BUTTON_COLS - 2;
    int col = bx / c->cell_w + (BUTTON_COLS - len) / 2;
    canvas_puts(c, col, 0, label, PAL_BG);
}

static void title(canvas_t *c, const char *left, const char *right)
{
    canvas_fill_rect(c, 0, 0, c->w, c->cell_h, PAL_TITLE_BG);
    canvas_puts(c, 1, 0, left, PAL_FG);
    if (right == NULL) return;

    /* Sit left of the button, and drop the note rather than overlap the
       heading if there is no room. */
    int col = c->cols - BUTTON_COLS - 1 - (int)strlen(right);
    if (col > (int)strlen(left) + 2)
        canvas_puts(c, col, 0, right, PAL_FG);
}

/* Warns in the build if a footer could ever be clipped at the right edge. */
static void footer_fit(canvas_t *c, char *text)
{
    if ((int)strlen(text) > c->cols - 1) text[c->cols - 1] = '\0';
}

/* A footnote explaining what was actually measured. Without it a bar chart is
   decoration: the reader cannot tell tokens from calls, or which window. */
static void footer(canvas_t *c, char *text)
{
    footer_fit(c, text);
    canvas_puts(c, 1, c->rows - 1, text, PAL_DIM);
}

static void right_text(canvas_t *c, int row, int right_col, const char *s,
                       uint16_t colour)
{
    canvas_puts(c, right_col - (int)strlen(s), row, s, colour);
}

void views_stats(canvas_t *c, const ud_view_t *d, float t)
{
    canvas_clear(c);
    char head[24];
    if (d->host_count > 1)
        snprintf(head, sizeof head, "%d machines", d->host_count);
    else
        snprintf(head, sizeof head, "tokens");
    canvas_clear(c);
    title(c, "USAGE BY MODEL", head);
    button(c, d);

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
        int width = (int)((double)total / (double)peak * bar_max * t);
        if (width < 2 && total > 0 && t >= 1.0f) width = 2;
        canvas_fill_rect(c, bar_x, row * c->cell_h + c->cell_h / 4,
                         width, c->cell_h / 2, pal_accent(i));

        char value[16];
        human(total, value, sizeof value);
        right_text(c, row, c->cols - 1, value, PAL_FG);
    }

    char note[128], total[16];
    human(grand, total, sizeof total);
    snprintf(note, sizeof note,
             "bar = output + cache-read tokens   %d models   %s total",
             d->model_count, total);
    footer(c, note);
}

void views_daily(canvas_t *c, const ud_view_t *d, float t)
{
    canvas_clear(c);

    if (d->day_count == 0) {
        title(c, "TOKENS PER DAY", NULL);
        button(c, d);
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    char range[48];
    if (d->host_count > 1)
        snprintf(range, sizeof range, "%s to %s   %d machines",
                 d->days[0].label, d->days[d->day_count - 1].label,
                 d->host_count);
    else
        snprintf(range, sizeof range, "%s to %s",
                 d->days[0].label, d->days[d->day_count - 1].label);
    title(c, "TOKENS PER DAY", range);
    button(c, d);

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
        canvas_fill_rect(c, bx, bottom - h, bar_w, h, colour);
        if (h >= 3)
            canvas_fill_rect(c, bx, bottom - h, bar_w, 2, pal_lighten(colour));

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
        human(peak, plab, sizeof plab);
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
    human(peak, peak_lab, sizeof peak_lab);
    human(grand / (uint64_t)d->day_count, avg, sizeof avg);
    human(grand, total, sizeof total);
    canvas_puts(c, 41, row, "bar = in+out+cache", PAL_DIM);

    char note[128];
    snprintf(note, sizeof note,
             "peak %s on %s   avg %s/day   %s over %d days",
             peak_lab, d->days[peak_i].label, avg, total, d->day_count);
    footer(c, note);
}

void views_today(canvas_t *c, const usagedata_t *d)
{
    canvas_clear(c);
    title(c, "TODAY", d->date[0] ? d->date : NULL);

    if (d->sun[0] == '\0' && d->moon_name[0] == '\0') {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-today.sh", PAL_DIM);
        return;
    }

    /* The moon gets the left third, drawn rather than described. */
    int r = (c->h - 5 * c->cell_h) / 2;
    if (r > 90) r = 90;
    int cx = 2 * c->cell_w + r;
    int cy = 2 * c->cell_h + r;
    canvas_moon(c, cx, cy, r, d->moon_phase, PAL_FG, 0x2124);

    int col = (cx + r) / c->cell_w + 2;
    if (d->moon_name[0])
        canvas_puts(c, col, 3, d->moon_name, PAL_A4);

    if (d->sun[0]) {
        canvas_puts(c, col, 6, "SUN", PAL_DIM);
        canvas_puts(c, col, 7, d->sun, PAL_FG);
    }
    if (d->dayinfo[0]) {
        canvas_puts(c, col, 10, "DAY", PAL_DIM);
        canvas_puts(c, col, 11, d->dayinfo, PAL_FG);
    }
    if (d->holiday[0]) {
        canvas_puts(c, col, 14, "NEXT HOLIDAY", PAL_DIM);
        canvas_puts(c, col, 15, d->holiday, PAL_A1);
    }
}
