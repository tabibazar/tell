#include "views.h"

#include "palette.h"
#include "textwrap.h"
#include "timecalc.h"

#include <stdio.h>
#include <string.h>

static void dot(canvas_t *c, int col, int row, uint16_t colour);

/* Compact magnitude: a billion has to fit in a narrow column. */
static void human(uint64_t n, char *out, int size)
{
    if (n >= 1000000000ULL) snprintf(out, size, "%.1fB", n / 1e9);
    else if (n >= 1000000ULL) snprintf(out, size, "%.1fM", n / 1e6);
    else if (n >= 1000ULL) snprintf(out, size, "%.0fK", n / 1e3);
    else snprintf(out, size, "%llu", (unsigned long long)n);
}

/* How long ago the newest machine sent data. Shown rather than used to expire
   anything: stale numbers are more useful when labelled than when deleted. */
static void freshness(const ud_view_t *d, int64_t now_us, char *out, int size)
{
    if (d->updated_us <= 0) { snprintf(out, size, "no data"); return; }

    long secs = (long)((now_us - d->updated_us) / 1000000);
    if (secs < 0) secs = 0;
    if (secs < 90) snprintf(out, size, "updated %lds ago", secs);
    else if (secs < 5400) snprintf(out, size, "updated %ldm ago", secs / 60);
    else snprintf(out, size, "updated %ldh ago", secs / 3600);
}

static void title(canvas_t *c, const char *left, const char *right)
{
    canvas_fill_rect(c, 0, 0, c->w, c->cell_h, PAL_TITLE_BG);
    canvas_puts(c, 1, 0, left, PAL_FG);
    if (right == NULL) return;

    /* Sit left of the button, and drop the note rather than overlap the
       heading if there is no room. */
    int col = c->cols - 1 - (int)strlen(right);
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

void views_stats(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    char head[48], fresh[24];
    freshness(d, now_us, fresh, sizeof fresh);
    if (d->host_count > 1)
        snprintf(head, sizeof head, "%d machines   %s", d->host_count, fresh);
    else
        snprintf(head, sizeof head, "%s", fresh);
    title(c, "USAGE BY MODEL", head);

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

void views_daily(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);

    if (d->day_count == 0) {
        title(c, "TOKENS PER DAY", NULL);
            canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    char range[64], fresh[24];
    freshness(d, now_us, fresh, sizeof fresh);
    snprintf(range, sizeof range, "%s to %s   %s",
             d->days[0].label, d->days[d->day_count - 1].label, fresh);
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

/* "18d 20h 19m", dropping the units that are zero from the left. */
static void duration(uint32_t secs, char *out, int size)
{
    unsigned d = secs / 86400u, h = (secs % 86400u) / 3600u, m = (secs % 3600u) / 60u;
    if (d) snprintf(out, size, "%ud %uh %um", d, h, m);
    else if (h) snprintf(out, size, "%uh %um", h, m);
    else snprintf(out, size, "%um", m);
}

/* "Aug 27" for a day count. */
static void day_name(int32_t days, char *out, int size)
{
    int y, m, d;
    timecalc_civil(days, &y, &m, &d);
    snprintf(out, size, "%s %d", timecalc_month_abbr(m), d);
}

/* A dim label followed by a coloured value, as one line. */
static void labelled(canvas_t *c, int col, int row, const char *label,
                     const char *value, uint16_t colour)
{
    canvas_puts(c, col, row, label, PAL_DIM);
    canvas_puts(c, col + (int)strlen(label), row, value, colour);
}

/* The heat grid's geometry: 53 week columns at their own pitch, wider than a
   text column, so the map is a mosaic of touching cells rather than a row of
   glyphs. Rows keep the text pitch so the weekday labels line up. */
#define HEAT_PITCH_X 14
#define HEAT_GAP     1

/* One cell of the map at a pixel position. Empty days get a dot, so they
   still read as days rather than as gaps in the map. */
static void heat_cell(canvas_t *c, int x, int y, int level)
{
    int w = HEAT_PITCH_X - HEAT_GAP, h = c->cell_h - HEAT_GAP;
    if (level > 0) canvas_fill_rect(c, x, y, w, h, pal_heat(level));
    else canvas_fill_rect(c, x + w / 2 - 1, y + h / 2 - 1, 2, 2, pal_heat(0));
}

void views_year(canvas_t *c, const ud_view_t *d, int64_t now_us)
{
    canvas_clear(c);
    char fresh[24];
    freshness(d, now_us, fresh, sizeof fresh);
    title(c, "LAST 12 MONTHS", fresh);

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
        heat_cell(c, x0 + (i / 7) * HEAT_PITCH_X, y0 + (i % 7) * c->cell_h,
                  y->level[i]);

    int row = grid_row + 7;
    canvas_puts_px(c, x0, row * c->cell_h, "Less", PAL_DIM);
    int lx = x0 + 5 * c->cell_w;
    for (int l = 1; l <= PAL_HEAT_STEPS; l++, lx += HEAT_PITCH_X)
        heat_cell(c, lx, row * c->cell_h, l);
    canvas_puts_px(c, lx + c->cell_w, row * c->cell_h, "More", PAL_DIM);

    /* The figures, two columns like the original. */
    const int left = 1, right = c->cols / 2 + 1;
    const uint16_t hi = pal_heat(PAL_HEAT_STEPS);
    char buf[32];
    uint64_t total = y->tok[0] + y->tok[1] + y->tok[2] + y->tok[3];

    row += 2;
    labelled(c, left, row, "Favorite model: ", y->fav[0] ? y->fav : "-", hi);
    human(total, buf, sizeof buf);
    labelled(c, right, row, "Total tokens: ", buf, hi);

    row += 2;
    snprintf(buf, sizeof buf, "%lu", (unsigned long)y->sessions);
    labelled(c, left, row, "Sessions: ", buf, hi);
    duration(y->longest_secs, buf, sizeof buf);
    labelled(c, right, row, "Longest session: ", buf, hi);

    row++;
    snprintf(buf, sizeof buf, "%d/%d", y->active_days, y->span_days);
    labelled(c, left, row, "Active days: ", buf, hi);
    snprintf(buf, sizeof buf, "%d day%s", y->longest_streak,
             y->longest_streak == 1 ? "" : "s");
    labelled(c, right, row, "Longest streak: ", buf, hi);

    row++;
    if (y->peak_index >= 0) day_name(y->start + y->peak_index, buf, sizeof buf);
    else snprintf(buf, sizeof buf, "-");
    labelled(c, left, row, "Most active day: ", buf, hi);
    snprintf(buf, sizeof buf, "%d day%s", y->current_streak,
             y->current_streak == 1 ? "" : "s");
    labelled(c, right, row, "Current streak: ", buf, hi);

    row++;
    char in[16], out[16], cr[16], cw[16], line[128];
    human(y->tok[0], in, sizeof in);
    human(y->tok[1], out, sizeof out);
    human(y->tok[2], cr, sizeof cr);
    human(y->tok[3], cw, sizeof cw);
    snprintf(line, sizeof line, "In %s . Out %s . Cache %s read . %s write",
             in, out, cr, cw);
    footer_fit(c, line);
    canvas_puts(c, left, row, line, PAL_DIM);

    if (y->estimated > 0) {
        snprintf(line, sizeof line,
                 "%d older day%s estimated from message counts; the rest measured",
                 y->estimated, y->estimated == 1 ? "" : "s");
        footer_fit(c, line);
        canvas_puts(c, left, row + 1, line, PAL_DIM);
    }

    snprintf(line, sizeof line,
             "cell = a day's tokens, shaded by quartile   figures = all time");
    footer(c, line);
}

/* "$1,234" from a hundred dollars up, "$12.34" below. */
static void money(uint64_t cents, char *out, int size)
{
    if (cents < 10000) {
        snprintf(out, size, "$%llu.%02llu", (unsigned long long)(cents / 100),
                 (unsigned long long)(cents % 100));
        return;
    }
    char digits[24];
    snprintf(digits, sizeof digits, "%llu", (unsigned long long)(cents / 100));
    int n = (int)strlen(digits), pos = 0;
    if (pos < size - 1) out[pos++] = '$';
    for (int i = 0; i < n && pos < size - 1; i++) {
        if (i > 0 && (n - i) % 3 == 0 && pos < size - 1) out[pos++] = ',';
        out[pos++] = digits[i];
    }
    out[pos] = '\0';
}

void views_cost(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    char fresh[24];
    freshness(d, now_us, fresh, sizeof fresh);
    title(c, "IF THIS WERE THE API", fresh);

    const ud_cost_view_t *k = &d->cost;
    if (!k->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    const int left = 1, right = c->cols / 2 + 1;
    const uint16_t hi = pal_heat(PAL_HEAT_STEPS);
    char buf[32];

    money(k->total, buf, sizeof buf);
    labelled(c, left, 2, "All time: ", buf, hi);
    money(k->last30, buf, sizeof buf);
    labelled(c, right, 2, "Last 30 days: ", buf, hi);
    money(k->last30 / 30, buf, sizeof buf);
    labelled(c, left, 3, "Per day, last 30: ", buf, hi);
    money(k->last7, buf, sizeof buf);
    labelled(c, right, 3, "Last 7 days: ", buf, hi);
    if (k->plan > 0) {
        char plan[24];
        money(k->plan, plan, sizeof plan);
        snprintf(buf, sizeof buf, "%s/mo", plan);
        labelled(c, left, 4, "Plan: ", buf, PAL_FG);
        snprintf(buf, sizeof buf, "%.1fx", (double)k->last30 / (double)k->plan);
        labelled(c, right, 4, "Last 30 days vs plan: ", buf, PAL_FG);
    }

    /* A bar per model, as on the tokens page, so the two read alike. */
    canvas_puts(c, left, 6, "model", PAL_DIM);
    canvas_puts(c, 17, 6, "share of all time", PAL_DIM);
    right_text(c, 6, c->cols - 1, "cost", PAL_DIM);
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
        money(k->models[i].cents, buf, sizeof buf);
        right_text(c, row, c->cols - 1, buf, PAL_FG);
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
        money(day_peak / 10, m, sizeof m);
        day_name(k->start + peak_i, dn, sizeof dn);
        snprintf(note, sizeof note, "peak %s on %s", m, dn);
        right_text(c, head_row, c->cols - 1, note, PAL_PEAK);
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
            day_name(k->start + i, lab, sizeof lab);
            int col = (px0 + i * slot) / c->cell_w;
            if (col + (int)strlen(lab) <= c->cols)
                canvas_puts(c, col, date_row, lab, PAL_DIM);
        }
    }
    (void)day_sum;

    char note[128];
    snprintf(note, sizeof note,
             "API list prices per Mtok; cache writes at 5-min rate unless 1h");
    footer(c, note);
}

static const char *const DOW_SHORT[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
static const char *const DOW_LONG[7] = {
    "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"
};

/* The rhythm grid: 24 hour columns at their own pitch across the width, and
   seven weekday rows taller than a text row so the map fills the top half. */
#define RHY_PITCH_X 31
#define RHY_PITCH_Y 36
#define RHY_GAP 1

void views_rhythm(canvas_t *c, const ud_view_t *d, int64_t now_us)
{
    canvas_clear(c);
    const ud_rhythm_view_t *r = &d->rhythm;
    char head[48];
    if (r->present) {
        snprintf(head, sizeof head, "%llu messages over %d days",
                 (unsigned long long)r->msgs, r->days);
        title(c, "WHEN YOU WORK", head);
    } else {
        title(c, "WHEN YOU WORK", NULL);
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }
    (void)now_us;

    const int x0 = 4 * c->cell_w + 4;
    const int y0 = 2 * c->cell_h;
    for (int h = 0; h < 24; h += 3) {
        char lab[4];
        snprintf(lab, sizeof lab, "%d", h);
        canvas_puts_px(c, x0 + h * RHY_PITCH_X, y0 - c->cell_h, lab, PAL_DIM);
    }
    for (int dow = 0; dow < 7; dow++) {
        int y = y0 + dow * RHY_PITCH_Y;
        canvas_puts_px(c, 6, y + (RHY_PITCH_Y - c->cell_h) / 2, DOW_SHORT[dow], PAL_DIM);
        for (int h = 0; h < 24; h++) {
            int x = x0 + h * RHY_PITCH_X;
            int level = r->level[dow * 24 + h];
            int w = RHY_PITCH_X - RHY_GAP, hh = RHY_PITCH_Y - RHY_GAP;
            if (level > 0) canvas_fill_rect(c, x, y, w, hh, pal_heat(level));
            else canvas_fill_rect(c, x + w / 2 - 1, y + hh / 2 - 1, 2, 2, pal_heat(0));
        }
    }

    /* Legend under the map, then the figures. */
    int ly = y0 + 7 * RHY_PITCH_Y + 6;
    canvas_puts_px(c, x0, ly, "Less", PAL_DIM);
    int lx = x0 + 5 * c->cell_w;
    for (int l = 1; l <= PAL_HEAT_STEPS; l++, lx += HEAT_PITCH_X)
        heat_cell(c, lx, ly, l);
    canvas_puts_px(c, lx + c->cell_w, ly, "More", PAL_DIM);

    const uint16_t hi = pal_heat(PAL_HEAT_STEPS);
    const int left = 1, right = c->cols / 2 + 1;
    int row = (ly + c->cell_h) / c->cell_h + 1;
    char buf[40];
    snprintf(buf, sizeof buf, "%02d:00-%02d:00", r->busiest_hour, (r->busiest_hour + 1) % 24);
    labelled(c, left, row, "Busiest hour: ", buf, hi);
    labelled(c, right, row, "Busiest day: ", DOW_LONG[r->busiest_dow], hi);
    row++;
    snprintf(buf, sizeof buf, "%d%%", r->night_pct);
    labelled(c, left, row, "Nights (22-06): ", buf, hi);
    snprintf(buf, sizeof buf, "%d%%", r->weekend_pct);
    labelled(c, right, row, "Weekends: ", buf, hi);
    row++;
    snprintf(buf, sizeof buf, "%s %02d:00, %lu messages", DOW_SHORT[r->peak_dow],
             r->peak_hour, (unsigned long)r->cell[r->peak_dow * 24 + r->peak_hour]);
    labelled(c, left, row, "Peak hour: ", buf, hi);

    char note[96];
    snprintf(note, sizeof note,
             "cell = messages in that weekday hour, shaded by quartile");
    footer(c, note);
}

/* "1h 21m" or "14m" or "38s". */
static void age(uint32_t secs, char *out, int size)
{
    unsigned long v = secs;
    if (v >= 86400) snprintf(out, size, "%lud %luh", v / 86400, (v % 86400) / 3600);
    else if (v >= 3600) snprintf(out, size, "%luh %lum", v / 3600, (v % 3600) / 60);
    else if (v >= 60) snprintf(out, size, "%lum", v / 60);
    else snprintf(out, size, "%lus", v);
}


void views_now(canvas_t *c, const ud_view_t *d, int64_t now_us)
{
    canvas_clear(c);
    char fresh[24];
    freshness(d, now_us, fresh, sizeof fresh);
    title(c, "TODAY, LIVE", fresh);

    const ud_now_view_t *n = &d->now;
    if (!n->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    const uint16_t hi = pal_heat(PAL_HEAT_STEPS);
    const int left = 1, right = c->cols / 2 + 1;
    char buf[64], a[24];

    /* The status line first: it is what a glance is for. */
    if (n->have_last) {
        int64_t since = n->last_secs + (now_us - n->last_sent_us) / 1000000;
        if (since < 0) since = 0;
        age((uint32_t)since, a, sizeof a);
        if (since < UD_BUSY_SECS) {
            canvas_puts(c, left, 2, "Claude is busy", hi);
            snprintf(buf, sizeof buf, "   last message %s ago", a);
            canvas_puts(c, left + 14, 2, buf, PAL_DIM);
        } else {
            canvas_puts(c, left, 2, "Claude is idle", PAL_FG);
            snprintf(buf, sizeof buf, "   last message %s ago", a);
            canvas_puts(c, left + 14, 2, buf, PAL_DIM);
        }
        snprintf(buf, sizeof buf, "on %s with %s", n->project[0] ? n->project : "?",
                 n->model[0] ? n->model : "?");
        canvas_puts(c, left, 3, buf, PAL_DIM);
    } else {
        canvas_puts(c, left, 2, "Nothing recorded yet today", PAL_DIM);
    }

    int row = 5;
    human(n->tokens, a, sizeof a);
    labelled(c, left, row, "Tokens today: ", a, hi);
    money(n->cost, a, sizeof a);
    labelled(c, right, row, "Cost today: ", a, hi);
    row++;
    snprintf(a, sizeof a, "%lu", (unsigned long)n->msgs);
    labelled(c, left, row, "Messages: ", a, hi);
    snprintf(a, sizeof a, "%lu", (unsigned long)n->sessions);
    labelled(c, right, row, "Sessions: ", a, hi);
    row++;
    if (n->have_last) {
        age(n->session_secs, a, sizeof a);
        labelled(c, left, row, "Current session: ", a, hi);
    }
    human(n->avg, a, sizeof a);
    labelled(c, right, row, "Typical day: ", a, hi);

    /* Today against a typical day, as one bar with a mark for typical. */
    row += 2;
    canvas_puts(c, left, row, "today against a typical day", PAL_DIM);
    uint64_t scale = n->tokens > n->avg ? n->tokens : n->avg;
    if (scale == 0) scale = 1;
    int bx = left * c->cell_w, bw = (c->cols - 2) * c->cell_w;
    int by = (row + 1) * c->cell_h + 4, bh = 2 * c->cell_h - 8;
    canvas_fill_rect(c, bx, by, bw, bh, 0x2124);
    int today_w = (int)((double)n->tokens / (double)scale * bw);
    canvas_fill_rect(c, bx, by, today_w, bh, hi);
    if (n->avg > 0) {
        int ax = bx + (int)((double)n->avg / (double)scale * bw);
        if (ax >= bx + bw) ax = bx + bw - 2;
        canvas_fill_rect(c, ax, by - 4, 2, bh + 8, PAL_FG);
        int lx = ax - 3 * c->cell_w;             /* centred under the mark */
        if (lx + 7 * c->cell_w > bx + bw) lx = bx + bw - 7 * c->cell_w;
        if (lx < bx) lx = bx;
        canvas_puts_px(c, lx, by + bh + 4, "typical", PAL_FG);
    }
    if (n->avg > 0) {
        snprintf(buf, sizeof buf, "%d%% of a typical day so far",
                 (int)((n->tokens * 100 + n->avg / 2) / n->avg));
        canvas_puts(c, left, row + 4, buf, PAL_DIM);
    }

    char note[96];
    snprintf(note, sizeof note, "from the transcripts, refreshed every minute");
    footer(c, note);
}

void views_projects(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    const ud_projects_view_t *p = &d->projects;
    char head[48], fresh[24];
    freshness(d, now_us, fresh, sizeof fresh);
    if (p->present) snprintf(head, sizeof head, "%d days   %s", p->days, fresh);
    else snprintf(head, sizeof head, "%s", fresh);
    title(c, "BY PROJECT", head);
    if (!p->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    canvas_puts(c, 1, 1, "project", PAL_DIM);
    canvas_puts(c, 17, 1, "share of tokens", PAL_DIM);
    right_text(c, 1, 52, "tokens", PAL_DIM);
    right_text(c, 1, c->cols - 1, "cost", PAL_DIM);

    int bar_x = 17 * c->cell_w;
    int bar_max = (44 - 17) * c->cell_w;
    uint64_t peak = 1;
    for (int i = 0; i < p->count; i++)
        if (p->rows[i].tokens > peak) peak = p->rows[i].tokens;

    uint32_t sessions = 0, msgs = 0;
    uint64_t cents = 0;
    for (int i = 0; i < p->count && 2 + i < c->rows - 4; i++) {
        const ud_project_t *r = &p->rows[i];
        int row = 2 + i;
        canvas_puts(c, 1, row, r->name, PAL_FG);
        int width = (int)((double)r->tokens / (double)peak * bar_max * t);
        if (width < 2 && r->tokens > 0 && t >= 1.0f) width = 2;
        canvas_fill_rect(c, bar_x, row * c->cell_h + c->cell_h / 4, width,
                         c->cell_h / 2, pal_accent(i));
        char buf[24];
        human(r->tokens, buf, sizeof buf);
        right_text(c, row, 52, buf, PAL_FG);
        money(r->cents, buf, sizeof buf);
        right_text(c, row, c->cols - 1, buf, PAL_FG);
        sessions += r->sessions;
        msgs += r->msgs;
        cents += r->cents;
    }

    /* Shares, as text, under the table: the bars are relative to the biggest
       project, which is the readable choice, but the share is the number. */
    int row = 2 + p->count + 1;
    if (row < c->rows - 3 && p->total_tokens > 0) {
        char line[128] = "";
        int used = 0;
        for (int i = 0; i < p->count && i < 4; i++) {
            int pct = (int)((p->rows[i].tokens * 100 + p->total_tokens / 2) / p->total_tokens);
            used += snprintf(line + used, sizeof line - (size_t)used, "%s%s %d%%",
                             i ? "  .  " : "", p->rows[i].name, pct);
            if (used >= (int)sizeof line - 1) break;
        }
        footer_fit(c, line);
        canvas_puts(c, 1, row, line, PAL_DIM);
    }

    char note[128], tot[24], cost[24];
    human(p->total_tokens, tot, sizeof tot);
    money(cents, cost, sizeof cost);
    snprintf(note, sizeof note, "%d projects   %lu sessions   %lu messages   %s   %s",
             p->count, (unsigned long)sessions, (unsigned long)msgs, tot, cost);
    footer_fit(c, note);
    canvas_puts(c, 1, c->rows - 2, note, PAL_DIM);
    snprintf(note, sizeof note,
             "bar = in+out+cache tokens, from this Mac's transcripts only");
    footer(c, note);
}

void views_cache(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    char fresh[24];
    freshness(d, now_us, fresh, sizeof fresh);
    title(c, "CACHE EFFICIENCY", fresh);

    const ud_cache_view_t *k = &d->cache;
    if (!k->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    const uint16_t hi = pal_heat(PAL_HEAT_STEPS);
    const int left = 1, right = c->cols / 2 + 1;
    char buf[48], a[24];

    snprintf(buf, sizeof buf, "%d%%", k->hit_pct);
    labelled(c, left, 2, "Input served from cache: ", buf, hi);
    money(k->saved, a, sizeof a);
    labelled(c, right, 2, "Saved by caching: ", a, hi);
    money(k->saved + k->cost, a, sizeof a);
    labelled(c, left, 3, "Without caching: ", a, hi);
    if (k->cost > 0) {
        snprintf(buf, sizeof buf, "%.1fx what it cost", (double)(k->saved + k->cost) / (double)k->cost);
        canvas_puts(c, right, 3, buf, PAL_DIM);
    }

    /* Per model: a bar on a fixed 0..100% scale, so bars are comparable. */
    canvas_puts(c, left, 5, "model", PAL_DIM);
    canvas_puts(c, 17, 5, "share of input from cache", PAL_DIM);
    right_text(c, 5, c->cols - 1, "saved", PAL_DIM);
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
        money(m->saved_cents, a, sizeof a);
        right_text(c, row, c->cols - 1, a, PAL_FG);
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
        right_text(c, yy / c->cell_h, gutter / c->cell_w - 1, buf, PAL_DIM);
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
            day_name(k->start + i, lab, sizeof lab);
            int col = (gutter + plot_w * i / k->len) / c->cell_w;
            if (col + (int)strlen(lab) <= c->cols) canvas_puts(c, col, date_row, lab, PAL_DIM);
        }
    } else {
        canvas_puts(c, gutter / c->cell_w + 2, 15, "no daily data", PAL_DIM);
    }

    char note[128];
    snprintf(note, sizeof note,
             "saved = cached tokens at input price minus at cache-read price");
    footer(c, note);
}

/* A step line of `n` daily values on a plot, values below zero skipped. Axis
   labels at 0, half and full of `top`; dates a fortnight apart beneath. */
static void step_plot(canvas_t *c, const int64_t *vals, int n, int64_t lo,
                      int64_t top, int32_t start_day, int plot_top_row,
                      int date_row, uint16_t colour, float t,
                      void (*label)(int64_t, char *, int))
{
    int gutter = 6 * c->cell_w;
    int py0 = plot_top_row * c->cell_h + 4;
    int bottom = date_row * c->cell_h - 4;
    int plot_h = bottom - py0, plot_w = c->w - gutter - 2 * c->cell_w;
    int64_t span = top - lo > 0 ? top - lo : 1;
    for (int q = 0; q <= 2; q++) {
        int yy = bottom - plot_h * q / 2;
        canvas_fill_rect(c, gutter, yy, plot_w, 1, PAL_DIM);
        char lab[16];
        label(lo + span * q / 2, lab, sizeof lab);
        right_text(c, yy / c->cell_h, gutter / c->cell_w - 1, lab, PAL_DIM);
    }
    if (n <= 0) {
        canvas_puts(c, gutter / c->cell_w + 2, plot_top_row + 2, "no daily data", PAL_DIM);
        return;
    }
    int prev_y = -1;
    for (int i = 0; i < n; i++) {
        int x0 = gutter + plot_w * i / n, x1 = gutter + plot_w * (i + 1) / n;
        if (vals[i] < 0) { prev_y = -1; continue; }
        int64_t above = vals[i] - lo;
        if (above < 0) above = 0;
        if (above > span) above = span;
        int yy = bottom - (int)((double)plot_h * (double)above / (double)span * t);
        if (prev_y >= 0 && prev_y != yy) {
            int lo_y = prev_y < yy ? prev_y : yy;
            canvas_fill_rect(c, x0, lo_y, 2, (prev_y > yy ? prev_y : yy) - lo_y + 2, colour);
        }
        canvas_fill_rect(c, x0, yy, x1 - x0, 2, colour);
        prev_y = yy;
    }
    for (int i = 0; i < n; i += 14) {
        char lab[16];
        day_name(start_day + i, lab, sizeof lab);
        int col = (gutter + plot_w * i / n) / c->cell_w;
        if (col + (int)strlen(lab) <= c->cols) canvas_puts(c, col, date_row, lab, PAL_DIM);
    }
}

static void label_count(int64_t v, char *out, int size) { human((uint64_t)v, out, size); }

/* The next 1, 2 or 5 times a power of ten at or above v, so an axis ends on
   a round number and its midpoint reads as one too. */
static int64_t nice_top(int64_t v)
{
    if (v <= 0) return 1;
    int64_t mag = 1;
    while (mag * 10 <= v) mag *= 10;
    if (v <= mag) return mag;
    if (v <= 2 * mag) return 2 * mag;
    if (v <= 5 * mag) return 5 * mag;
    return 10 * mag;
}
static void label_pct(int64_t v, char *out, int size) { snprintf(out, size, "%lld%%", (long long)v); }

void views_tools(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    const ud_tools_view_t *tl = &d->tools;
    char head[48], fresh[24];
    freshness(d, now_us, fresh, sizeof fresh);
    if (tl->present) snprintf(head, sizeof head, "%d days   %s", tl->days, fresh);
    else snprintf(head, sizeof head, "%s", fresh);
    title(c, "TOOLS", head);
    if (!tl->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    canvas_puts(c, 1, 1, "tool", PAL_DIM);
    canvas_puts(c, 17, 1, "share of calls", PAL_DIM);
    right_text(c, 1, c->cols - 1, "calls", PAL_DIM);
    int bar_x = 17 * c->cell_w, bar_max = (46 - 17) * c->cell_w;
    uint32_t peak = 1;
    for (int i = 0; i < tl->count; i++) if (tl->rows[i].calls > peak) peak = tl->rows[i].calls;
    for (int i = 0; i < tl->count && i < 8; i++) {
        const ud_tool_t *r = &tl->rows[i];
        int row = 2 + i;
        canvas_puts(c, 1, row, r->name, PAL_FG);
        int width = (int)((double)r->calls / (double)peak * bar_max * t);
        if (width < 2 && r->calls > 0 && t >= 1.0f) width = 2;
        canvas_fill_rect(c, bar_x, row * c->cell_h + c->cell_h / 4, width, c->cell_h / 2,
                         pal_accent(i));
        char buf[24];
        if (tl->calls > 0) {
            snprintf(buf, sizeof buf, "%lu%%",
                     (unsigned long)(((uint64_t)r->calls * 100 + tl->calls / 2) / tl->calls));
            canvas_puts(c, 48, row, buf, PAL_DIM);
        }
        snprintf(buf, sizeof buf, "%lu", (unsigned long)r->calls);
        right_text(c, row, c->cols - 1, buf, PAL_FG);
    }

    char line[128];
    snprintf(line, sizeof line, "%lu calls   %.0f per session   %.2f per message",
             (unsigned long)tl->calls,
             tl->sessions ? (double)tl->calls / tl->sessions : 0.0,
             tl->msgs ? (double)tl->calls / tl->msgs : 0.0);
    footer_fit(c, line);
    canvas_puts(c, 1, 11, line, PAL_DIM);

    canvas_puts(c, 1, 12, "tool calls per day, last 60 days", PAL_DIM);
    int64_t vals[UD_MDAYS];
    int64_t top = 1;
    for (int i = 0; i < tl->len; i++) {
        vals[i] = (int64_t)tl->day[i];
        if (vals[i] > top) top = vals[i];
    }
    step_plot(c, vals, tl->len, 0, nice_top(top), tl->start, 13, 18,
              pal_heat(PAL_HEAT_STEPS), t, label_count);

    snprintf(line, sizeof line,
             "counts tool_use blocks; older days from Claude Code's own cache");
    footer(c, line);
}

void views_thinking(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    const ud_thinking_view_t *th = &d->thinking;
    char head[48], fresh[24];
    freshness(d, now_us, fresh, sizeof fresh);
    if (th->present) snprintf(head, sizeof head, "%d days   %s", th->days, fresh);
    else snprintf(head, sizeof head, "%s", fresh);
    title(c, "THINKING SHARE", head);
    if (!th->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    const uint16_t hi = pal_heat(PAL_HEAT_STEPS);
    const int left = 1, right = c->cols / 2 + 1;
    char buf[80], a[24], b[24];
    snprintf(buf, sizeof buf, "%d%% of output tokens", th->share_pct);
    labelled(c, left, 2, "Thinking: ", buf, hi);
    money(th->think_cents, a, sizeof a);
    labelled(c, right, 2, "Cost of thinking: ", a, hi);
    human(th->thinking, a, sizeof a);
    human(th->visible, b, sizeof b);
    snprintf(buf, sizeof buf, "%s thinking . %s visible", a, b);
    canvas_puts(c, left, 3, buf, PAL_DIM);
    money(th->out_cents, a, sizeof a);
    snprintf(buf, sizeof buf, "of %s spent on output", a);
    canvas_puts(c, right, 3, buf, PAL_DIM);

    canvas_puts(c, left, 5, "model", PAL_DIM);
    canvas_puts(c, 17, 5, "share of output that is thinking", PAL_DIM);
    right_text(c, 5, c->cols - 1, "thinking", PAL_DIM);
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
        human(m->thinking, a, sizeof a);
        right_text(c, row, c->cols - 1, a, PAL_FG);
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
    step_plot(c, vals, th->len, 0, top, th->start, 13, 18, hi, t, label_pct);

    char note[128];
    snprintf(note, sizeof note,
             "share = thinking / output tokens, where the transcript reports the split");
    footer(c, note);
}

/* "20 Aug 2026" for a day count, for records that may span years. */
static void full_date(int32_t days, char *out, int size)
{
    int y, m, d;
    timecalc_civil(days, &y, &m, &d);
    snprintf(out, size, "%d %s %d", d, timecalc_month_abbr(m), y);
}

void views_week(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    const ud_week_view_t *w = &d->week;
    char head[72], fresh[24];
    freshness(d, now_us, fresh, sizeof fresh);
    if (w->present && w->today > 0) {
        char dn[16];
        day_name(w->today, dn, sizeof dn);
        snprintf(head, sizeof head, "to %s   %s", dn, fresh);
    } else {
        snprintf(head, sizeof head, "%s", fresh);
    }
    title(c, "THIS WEEK VS LAST", head);
    if (!w->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    static const char *const labels[UD_WEEK_ROWS] = {
        "Tokens", "Cost", "Messages", "Sessions", "Tool calls", "Active days"
    };
    const int col_this = 30, col_last = 46, col_change = c->cols - 1;
    right_text(c, 2, col_this, "last 7 days", PAL_DIM);
    right_text(c, 2, col_last, "the 7 before", PAL_DIM);
    right_text(c, 2, col_change, "change", PAL_DIM);
    for (int r = 0; r < UD_WEEK_ROWS; r++) {
        int row = 3 + r;
        char a[24], b[24], ch[16];
        uint64_t now_v = w->this_week[r], then_v = w->last_week[r];
        if (r == 0) { human(now_v, a, sizeof a); human(then_v, b, sizeof b); }
        else if (r == 1) { money(now_v, a, sizeof a); money(then_v, b, sizeof b); }
        else {
            snprintf(a, sizeof a, "%llu", (unsigned long long)now_v);
            snprintf(b, sizeof b, "%llu", (unsigned long long)then_v);
        }
        canvas_puts(c, 1, row, labels[r], PAL_DIM);
        right_text(c, row, col_this, a, PAL_FG);
        right_text(c, row, col_last, b, PAL_DIM);
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
        right_text(c, row, col_change, ch, colour);
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
    footer(c, note);
}

void views_records(canvas_t *c, const ud_view_t *d, int64_t now_us)
{
    canvas_clear(c);
    const ud_records_view_t *r = &d->records;
    char head[72], fresh[24];
    freshness(d, now_us, fresh, sizeof fresh);
    if (r->present && r->since > 0) {
        char since[24];
        full_date(r->since, since, sizeof since);
        snprintf(head, sizeof head, "since %s   %s", since, fresh);
    } else {
        snprintf(head, sizeof head, "%s", fresh);
    }
    title(c, "RECORDS", head);
    if (!r->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    /* Each record: what it is, the value, and when. Unknown keys are ignored,
       so a newer collector can send more than this firmware shows. */
    struct { const char *key, *label; int kind; } rows[] = {
        { "bigday",   "Biggest day",               0 },   /* tokens */
        { "costday",  "Most expensive day",        1 },   /* cents */
        { "msgs",     "Most messages in a day",    2 },   /* count */
        { "streak",   "Longest streak",            3 },   /* days */
        { "session",  "Longest session",           4 },   /* seconds */
        { "toolsess", "Most tool calls, one session", 2 },
        { "response", "Biggest single response",   5 },   /* tokens, "tokens" */
        { "early",    "Earliest message",          6 },   /* minute of day */
        { "late",     "Latest message",            6 },
    };
    const uint16_t hi = pal_heat(PAL_HEAT_STEPS);
    int row = 2;
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        const ud_record_t *rec = usagedata_record(r, rows[i].key);
        if (rec == NULL) continue;
        char val[32], when[24];
        switch (rows[i].kind) {
        case 0: human(rec->value, val, sizeof val); strncat(val, " tokens", sizeof val - strlen(val) - 1); break;
        case 1: money(rec->value, val, sizeof val); break;
        case 3: snprintf(val, sizeof val, "%llu days", (unsigned long long)rec->value); break;
        case 4: duration((uint32_t)rec->value, val, sizeof val); break;
        case 5: human(rec->value, val, sizeof val); strncat(val, " tokens", sizeof val - strlen(val) - 1); break;
        case 6: snprintf(val, sizeof val, "%02llu:%02llu", (unsigned long long)rec->value / 60,
                         (unsigned long long)rec->value % 60); break;
        default: snprintf(val, sizeof val, "%llu", (unsigned long long)rec->value); break;
        }
        canvas_puts(c, 1, row, rows[i].label, PAL_DIM);
        canvas_puts(c, 31, row, val, hi);
        if (rec->day > 0) {
            full_date(rec->day, when, sizeof when);
            if (strcmp(rows[i].key, "streak") == 0) {
                char ending[32];
                snprintf(ending, sizeof ending, "ending %s", when);
                right_text(c, row, c->cols - 1, ending, PAL_DIM);
            } else {
                right_text(c, row, c->cols - 1, when, PAL_DIM);
            }
        }
        row += 2;
        if (row >= c->rows - 1) break;
    }

    char note[96];
    snprintf(note, sizeof note, "personal bests from the transcripts and Claude Code's cache");
    footer(c, note);
}

void views_runs(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    const ud_runs_view_t *r = &d->runs;
    char head[72], fresh[24];
    freshness(d, now_us, fresh, sizeof fresh);
    if (r->present) snprintf(head, sizeof head, "%d days   %s", r->days, fresh);
    else snprintf(head, sizeof head, "%s", fresh);
    title(c, "WHAT CLAUDE RUNS", head);
    if (!r->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    canvas_puts(c, 1, 1, "program", PAL_DIM);
    canvas_puts(c, 17, 1, "share of commands", PAL_DIM);
    right_text(c, 1, c->cols - 1, "commands", PAL_DIM);
    int bar_x = 17 * c->cell_w, bar_max = (46 - 17) * c->cell_w;
    uint32_t peak = 1;
    for (int i = 0; i < r->count; i++) if (r->rows[i].calls > peak) peak = r->rows[i].calls;
    int shown = r->count < 9 ? r->count : 9;
    for (int i = 0; i < shown; i++) {
        const ud_tool_t *row = &r->rows[i];
        int y = 2 + i;
        bool other = strcmp(row->name, "other") == 0;
        canvas_puts(c, 1, y, row->name, other ? PAL_DIM : PAL_FG);
        int width = (int)((double)row->calls / (double)peak * bar_max * t);
        if (width < 2 && row->calls > 0 && t >= 1.0f) width = 2;
        canvas_fill_rect(c, bar_x, y * c->cell_h + c->cell_h / 4, width, c->cell_h / 2,
                         other ? PAL_DIM : pal_accent(i));
        char buf[24];
        if (r->commands > 0) {
            snprintf(buf, sizeof buf, "%lu%%",
                     (unsigned long)(((uint64_t)row->calls * 100 + r->commands / 2) / r->commands));
            canvas_puts(c, 48, y, buf, PAL_DIM);
        }
        snprintf(buf, sizeof buf, "%lu", (unsigned long)row->calls);
        right_text(c, y, c->cols - 1, buf, PAL_FG);
    }

    /* What kind of work: one bar split by category, with a legend. */
    char line[128];
    snprintf(line, sizeof line, "%lu Bash calls   %lu commands   %.1f per call",
             (unsigned long)r->calls, (unsigned long)r->commands,
             r->calls ? (double)r->commands / r->calls : 0.0);
    footer_fit(c, line);
    canvas_puts(c, 1, 12, line, PAL_DIM);

    canvas_puts(c, 1, 13, "what kind of work", PAL_DIM);
    uint64_t total = 0;
    for (int k = 0; k < UD_RUN_CATS; k++) total += r->cats[k];
    int bx = c->cell_w, bw = (c->cols - 2) * c->cell_w;
    int by = 14 * c->cell_h + 4, bh = c->cell_h - 8;
    canvas_fill_rect(c, bx, by, bw, bh, 0x2124);
    int drawn = 0;
    for (int k = 0; k < UD_RUN_CATS && total > 0; k++) {
        int seg = (int)((double)r->cats[k] / (double)total * bw * t);
        if (k == UD_RUN_CATS - 1 && t >= 1.0f) seg = (int)(bw * t) - drawn;
        if (seg <= 0) continue;
        canvas_fill_rect(c, bx + drawn, by, seg, bh, k == UD_RUN_CATS - 1 ? PAL_DIM : pal_accent(k));
        if (seg > 2) canvas_fill_rect(c, bx + drawn + seg - 2, by, 2, bh, PAL_BG);   /* a gap */
        drawn += seg;
    }
    /* Legend in two rows of four, each with its share. */
    for (int k = 0; k < UD_RUN_CATS; k++) {
        int row = 15 + k / 4, col = 1 + (k % 4) * 16;
        dot(c, col, row, k == UD_RUN_CATS - 1 ? PAL_DIM : pal_accent(k));
        char buf[32];
        int pct = total ? (int)(((uint64_t)r->cats[k] * 100 + total / 2) / total) : 0;
        snprintf(buf, sizeof buf, "%s %d%%", usagedata_run_cats[k], pct);
        canvas_puts(c, col + 1, row, buf, PAL_FG);
    }

    snprintf(line, sizeof line,
             "each pipe stage and && step counts once, heredoc bodies not");
    footer(c, line);
}

static void label_secs(int64_t v, char *out, int size)
{
    if (v >= 3600) snprintf(out, size, "%lldh", (long long)(v / 3600));
    else if (v >= 60) snprintf(out, size, "%lldm", (long long)(v / 60));
    else snprintf(out, size, "%llds", (long long)v);
}

void views_turns(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    const ud_turns_view_t *tn = &d->turns;
    char head[72], fresh[24];
    freshness(d, now_us, fresh, sizeof fresh);
    if (tn->present) snprintf(head, sizeof head, "%d days   %s", tn->days, fresh);
    else snprintf(head, sizeof head, "%s", fresh);
    title(c, "TURNS", head);
    if (!tn->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    const uint16_t hi = pal_heat(PAL_HEAT_STEPS);
    const int left = 1, right = c->cols / 2 + 1;
    char a[24], buf[64];
    age(tn->first_med, a, sizeof a);
    labelled(c, left, 2, "First reply, typically: ", a, hi);
    age(tn->turn_med, a, sizeof a);
    labelled(c, right, 2, "A turn, typically: ", a, hi);
    age(tn->first_p90, a, sizeof a);
    labelled(c, left, 3, "9 in 10 replies within: ", a, PAL_FG);
    age(tn->turn_p90, a, sizeof a);
    labelled(c, right, 3, "9 in 10 turns within: ", a, PAL_FG);

    age(tn->longest, a, sizeof a);
    labelled(c, left, 5, "Longest turn: ", a, hi);
    if (tn->longest_day > 0) {
        char when[24];
        full_date(tn->longest_day, when, sizeof when);
        snprintf(buf, sizeof buf, "on %s", when);
        canvas_puts(c, left + 14 + (int)strlen(a) + 1, 5, buf, PAL_DIM);
    }
    if (tn->turns > 0) {
        snprintf(buf, sizeof buf, "%lu turns, %lu interrupted by you (%lu%%)",
                 (unsigned long)tn->turns, (unsigned long)tn->interrupted,
                 (unsigned long)(((uint64_t)tn->interrupted * 100 + tn->turns / 2) / tn->turns));
        canvas_puts(c, left, 6, buf, PAL_FG);
    }

    /* A turn starts at your prompt and ends at Claude's last message before
       your next one, so it counts the tool calls in between. */
    canvas_puts(c, left, 8, "a turn runs from your prompt to Claude's last message", PAL_DIM);
    canvas_puts(c, left, 9, "before your next one, tool calls included", PAL_DIM);

    canvas_puts(c, left, 11, "typical turn per day, last 60 days", PAL_DIM);
    /* The axis follows the days in general, not one freak day: if the largest
       is more than three times the next, the next sets the scale and the
       freak is clipped at the top. */
    int64_t top = 0, second = 0;
    for (int i = 0; i < tn->len; i++) {
        if (tn->day[i] > top) { second = top; top = tn->day[i]; }
        else if (tn->day[i] > second) second = tn->day[i];
    }
    if (second > 0 && top > 3 * second) top = second;
    /* A round axis in seconds: the next 30s, minute, 2, 5, 10, 30 or 60 minutes. */
    static const int64_t steps[] = { 30, 60, 120, 300, 600, 1800, 3600, 7200, 14400, 43200, 86400 };
    int64_t nice = steps[sizeof steps / sizeof steps[0] - 1];
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++)
        if (steps[i] >= top) { nice = steps[i]; break; }
    step_plot(c, tn->day, tn->len, 0, nice, tn->start, 12, 18, hi, t, label_secs);

    char note[96];
    snprintf(note, sizeof note, "typical = median; from the transcripts on this Mac");
    footer(c, note);
}

/* The menu: tiles four across, five down, one per available page, in tap
   order. Each is a raised plate the size of a fingertip several times over. */
#define MENU_COLS   4
#define MENU_TOP    30
#define MENU_PITCH  88
#define MENU_TILE_H 80

static const struct { page_t page; const char *name; } MENU_NAMES[] = {
    { PAGE_CLOCK,    "Clock" },
    { PAGE_NOW,      "Today, live" },
    { PAGE_STORY,    "Today's story" },
    { PAGE_WEEK,     "Week vs last" },
    { PAGE_STATS,    "By model" },
    { PAGE_TODAY,    "Almanac" },
    { PAGE_MODELS,   "Models per day" },
    { PAGE_PROJECTS, "By project" },
    { PAGE_YEAR,     "Last 12 months" },
    { PAGE_RHYTHM,   "When you work" },
    { PAGE_COST,     "API cost" },
    { PAGE_CACHE,    "Cache" },
    { PAGE_TOOLS,    "Tools" },
    { PAGE_RUNS,     "What it runs" },
    { PAGE_THINKING, "Thinking" },
    { PAGE_RECORDS,  "Records" },
    { PAGE_TURNS,    "Turns" },
    { PAGE_MESSAGE,  "Message" },
    { PAGE_DAILY,    "Tokens per day" },
    { PAGE_SETTINGS, "Settings" },
};

static const char *menu_name(page_t page)
{
    for (size_t i = 0; i < sizeof MENU_NAMES / sizeof MENU_NAMES[0]; i++)
        if (MENU_NAMES[i].page == page) return MENU_NAMES[i].name;
    return "?";
}

/* The nth tile's page, walking the available pages in order, skipping the
   menu itself. PAGE_COUNT when there is no nth tile. */
static page_t menu_tile_page(const pages_t *p, int n)
{
    int seen = 0;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (i == PAGE_MENU || !(p->available & PAGE_BIT(i))) continue;
        if (seen++ == n) return (page_t)i;
    }
    return PAGE_COUNT;
}

static void menu_tile_rect(canvas_t *c, int n, int *x, int *y, int *w, int *h)
{
    int tile_w = c->w / MENU_COLS;
    *x = (n % MENU_COLS) * tile_w + 4;
    *y = MENU_TOP + (n / MENU_COLS) * MENU_PITCH;
    *w = tile_w - 8;
    *h = MENU_TILE_H;
}

void views_menu(canvas_t *c, const pages_t *p)
{
    canvas_clear(c);
    title(c, "MENU", "tap a page");
    for (int n = 0; ; n++) {
        page_t page = menu_tile_page(p, n);
        if (page == PAGE_COUNT) break;
        int x, y, w, h;
        menu_tile_rect(c, n, &x, &y, &w, &h);
        if (y + h > c->h) break;
        canvas_fill_rect(c, x, y, w, h, 0x2124);
        canvas_fill_rect(c, x, y, w, 3, 0x4208);
        canvas_fill_rect(c, x, y + h, w, 2, 0x18C3);
        const char *name = menu_name(page);
        int tw = (int)strlen(name) * c->cell_w;
        canvas_puts_px(c, x + (w - tw) / 2, y + (h - c->cell_h) / 2, name,
                       page == p->current ? pal_heat(PAL_HEAT_STEPS) : PAL_FG);
    }
}

bool views_menu_hit(canvas_t *c, const pages_t *p, int px, int py, page_t *page)
{
    for (int n = 0; ; n++) {
        page_t candidate = menu_tile_page(p, n);
        if (candidate == PAGE_COUNT) return false;
        int x, y, w, h;
        menu_tile_rect(c, n, &x, &y, &w, &h);
        /* The gaps count too: a finger between two tiles meant one of them. */
        if (px >= x - 4 && px < x + w + 4 && py >= y - 4 && py < y + MENU_PITCH - 4) {
            *page = candidate;
            return true;
        }
    }
}

void views_story(canvas_t *c, const ud_view_t *d, int64_t now_us)
{
    canvas_clear(c);
    const ud_story_view_t *st = &d->story;
    char head[72], fresh[24];
    freshness(d, now_us, fresh, sizeof fresh);
    if (st->present && st->day > 0) {
        char when[24];
        full_date(st->day, when, sizeof when);
        snprintf(head, sizeof head, "%s   %s", when, fresh);
    } else {
        snprintf(head, sizeof head, "%s", fresh);
    }
    title(c, "TODAY'S STORY", head);
    if (!st->present || st->text[0] == '\0') {
        canvas_puts(c, 1, 2, "no story yet -- run tools/push-story.sh", PAL_DIM);
        return;
    }

    /* Prose, wrapped to sixty columns with a little air between lines. It
       is the one page that is read rather than glanced at. */
    char lines[TW_MAX_LINES][TW_MAX_COLS + 1];
    size_t n = textwrap(st->text, 60, 12, lines);
    int y = 2 * c->cell_h + 8;
    for (size_t i = 0; i < n; i++, y += c->cell_h + 8)
        canvas_puts_px(c, 2 * c->cell_w, y, lines[i], PAL_FG);

    char note[96];
    snprintf(note, sizeof note,
             "written by Claude from today's %lu prompts, every half hour",
             (unsigned long)st->prompts);
    footer(c, note);
}

/* A filled marker beside a legend or card entry. */
static void dot(canvas_t *c, int col, int row, uint16_t colour)
{
    canvas_fill_rect(c, col * c->cell_w + 3, row * c->cell_h + c->cell_h / 2 - 3,
                     6, 6, colour);
}

#define MODEL_LINES 3     /* series drawn; more would be unreadable */
#define MODEL_CARDS 4

void views_models(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    char fresh[24];
    freshness(d, now_us, fresh, sizeof fresh);
    title(c, "TOKENS PER DAY BY MODEL", fresh);

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
        human(peak * (uint64_t)q / 4, lab, sizeof lab);
        right_text(c, yy / c->cell_h, gutter / c->cell_w - 1, lab, PAL_DIM);
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
            day_name(d->mstart + i, lab, sizeof lab);
            int col = (gutter + plot_w * i / d->mlen) / c->cell_w;
            if (col + (int)strlen(lab) <= c->cols)
                canvas_puts(c, col, date_row, lab, PAL_DIM);
        }
    }

    /* Legend: which colour is which model. */
    int col = 1;
    for (int s = 0; s < lines; s++) {
        if (s > 0) { canvas_puts(c, col, legend_row, ".", PAL_DIM); col += 2; }
        dot(c, col, legend_row, pal_accent(s));
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

        dot(c, 1, row, pal_accent(i));
        canvas_puts(c, 2, row, m->name, PAL_FG);
        snprintf(buf, sizeof buf, " (%.1f%%)", 100.0 * (double)mine / (double)grand);
        canvas_puts(c, 2 + (int)strlen(m->name), row, buf, PAL_DIM);

        human(m->in, a, sizeof a);
        human(m->out, b, sizeof b);
        human(m->cread, cr, sizeof cr);
        human(m->cwrite, cw, sizeof cw);
        snprintf(buf, sizeof buf, "In %s . Out %s . Cache %s read . %s write",
                 a, b, cr, cw);
        footer_fit(c, buf);
        canvas_puts(c, 3, row + 1, buf, PAL_DIM);
    }

    char note[128];
    snprintf(note, sizeof note,
             "line = in+out+cache per day, last %d days   cards = all time",
             d->mlen > 0 ? d->mlen : UD_MDAYS);
    footer(c, note);
}

/* Settings layout: each setting takes a block of four text rows -- a label,
   two rows of buttons, a gap -- so three fit with room for a footer. Buttons
   are drawn two rows tall and hit-tested over three: one row is 24px, about
   4mm on this panel, and taps that missed it used to advance the page. */
#define SET_TOP_ROW     2
#define SET_BLOCK_ROWS  4
#define SET_BUTTON_ROWS 2
#define SET_HIT_ROWS    3
#define SET_LEFT_COL    1
#define SET_GAP_COLS    1

/* A button's text column and width in columns; the text sits inside one
   column of padding each side. */
static void set_button_span(int row, int choice, int *col, int *cols)
{
    const settings_row_t *r = settings_row(row);
    int at = SET_LEFT_COL;
    for (int i = 0; i < choice; i++)
        at += (int)strlen(r->choices[i]) + 2 + SET_GAP_COLS;
    *col = at;
    *cols = (int)strlen(r->choices[choice]) + 2;
}

void views_settings(canvas_t *c, const settings_t *s)
{
    canvas_clear(c);
    title(c, "SETTINGS", NULL);

    for (int row = 0; row < SETTINGS_ROWS; row++) {
        const settings_row_t *r = settings_row(row);
        int label_row = SET_TOP_ROW + row * SET_BLOCK_ROWS;
        canvas_puts(c, SET_LEFT_COL, label_row, r->label, PAL_DIM);

        int chosen = settings_choice(s, row);
        int y = (label_row + 1) * c->cell_h;
        int h = SET_BUTTON_ROWS * c->cell_h;
        for (int i = 0; i < r->count; i++) {
            int col, cols;
            set_button_span(row, i, &col, &cols);
            int x = col * c->cell_w, w = cols * c->cell_w;
            bool on = i == chosen;
            /* The chosen one is a raised amber block; the rest are dark
               plates, so all of them read as pressable but only one as set. */
            canvas_fill_rect(c, x, y, w, h, on ? PAL_A1 : 0x2124);
            canvas_fill_rect(c, x, y, w, 3, on ? pal_lighten(PAL_A1) : 0x4208);
            canvas_fill_rect(c, x, y + h, w, 2, on ? PAL_DIM : 0x2124);
            canvas_puts_px(c, x + c->cell_w, y + (h - c->cell_h) / 2,
                           r->choices[i], on ? PAL_BG : PAL_FG);
        }
    }

    char note[96];
    snprintf(note, sizeof note,
             "tap a value to set it -- kept across power cycles");
    canvas_puts(c, 1, c->rows - 2, note, PAL_DIM);
    snprintf(note, sizeof note,
             "elsewhere: right half = next page, left half = previous");
    footer(c, note);
}

bool views_settings_hit(canvas_t *c, int x, int y, int *row, int *choice)
{
    for (int r = 0; r < SETTINGS_ROWS; r++) {
        int top = (SET_TOP_ROW + r * SET_BLOCK_ROWS + 1) * c->cell_h;
        if (y < top || y >= top + SET_HIT_ROWS * c->cell_h) continue;
        const settings_row_t *sr = settings_row(r);
        for (int i = 0; i < sr->count; i++) {
            int col, cols;
            set_button_span(r, i, &col, &cols);
            /* Half the gap on either side counts too; there is no reason
               to make a tap between two buttons do something else. */
            int x0 = col * c->cell_w - (SET_GAP_COLS * c->cell_w) / 2;
            int x1 = (col + cols) * c->cell_w + (SET_GAP_COLS * c->cell_w) / 2;
            if (x >= x0 && x < x1) {
                *row = r;
                *choice = i;
                return true;
            }
        }
    }
    return false;
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
