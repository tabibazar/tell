#include "view_common.h"

#include <stdio.h>
#include <string.h>

/* Compact magnitude: a billion has to fit in a narrow column. */
void vw_human(uint64_t n, char *out, int size)
{
    if (n >= 1000000000ULL) snprintf(out, size, "%.1fB", n / 1e9);
    else if (n >= 1000000ULL) snprintf(out, size, "%.1fM", n / 1e6);
    else if (n >= 1000ULL) snprintf(out, size, "%.0fK", n / 1e3);
    else snprintf(out, size, "%llu", (unsigned long long)n);
}

/* How long ago the newest machine sent data. Shown rather than used to expire
   anything: stale numbers are more useful when labelled than when deleted. */
void vw_freshness(const ud_view_t *d, int64_t now_us, char *out, int size)
{
    if (d->updated_us <= 0) { snprintf(out, size, "no data"); return; }

    long secs = (long)((now_us - d->updated_us) / 1000000);
    if (secs < 0) secs = 0;
    if (secs < 90) snprintf(out, size, "updated %lds ago", secs);
    else if (secs < 5400) snprintf(out, size, "updated %ldm ago", secs / 60);
    else snprintf(out, size, "updated %ldh ago", secs / 3600);
}

static char s_strip[32];

void vw_set_clock(bool known, uint32_t local_secs, int utc_offset_min,
                  const char *tz)
{
    if (!known) { s_strip[0] = '\0'; return; }
    uint32_t local = local_secs % SECS_PER_DAY;
    /* UTC is the local time less the offset; it may sit on the other side
       of midnight, which the modulo handles. */
    long utc = ((long)local - (long)utc_offset_min * 60L) % (long)SECS_PER_DAY;
    if (utc < 0) utc += SECS_PER_DAY;
    snprintf(s_strip, sizeof s_strip, "%02lu:%02lu %s  %02ld:%02ld UTC",
             (unsigned long)(local / 3600), (unsigned long)((local % 3600) / 60),
             tz && tz[0] ? tz : "local", utc / 3600, (utc % 3600) / 60);
}

const char *vw_clock_strip(void)
{
    return s_strip;
}

void vw_menu_tab(canvas_t *c)
{
    int w = VW_TAB_COLS * c->cell_w;
    canvas_fill_rect(c, 0, 0, w, c->cell_h, PAL_A1);
    canvas_fill_rect(c, 0, 0, w, 2, pal_lighten(PAL_A1));
    canvas_puts(c, 1, 0, "MENU", PAL_BG);
}

bool vw_menu_tab_hit(canvas_t *c, int x, int y)
{
    return x < (VW_TAB_COLS + 2) * c->cell_w && y < 3 * c->cell_h;
}

void vw_title(canvas_t *c, const char *left, const char *right)
{
    canvas_fill_rect(c, 0, 0, c->w, c->cell_h, PAL_TITLE_BG);
    vw_menu_tab(c);
    canvas_puts(c, VW_TAB_COLS + 1, 0, left, PAL_FG);

    /* The clock strip takes the right edge when it is known; the page's own
       note sits left of it, and is dropped rather than overlapped if the
       heading leaves no room. */
    int edge = c->cols - 1;
    if (s_strip[0]) {
        int col = edge - (int)strlen(s_strip);
        canvas_puts(c, col, 0, s_strip, PAL_FG);
        edge = col - 2;
    }
    if (right == NULL) return;
    int col = edge - (int)strlen(right);
    if (col > VW_TAB_COLS + 1 + (int)strlen(left) + 1)
        canvas_puts(c, col, 0, right, PAL_DIM);
}

/* Warns in the build if a footer could ever be clipped at the right edge. */
void vw_footer_fit(canvas_t *c, char *text)
{
    if ((int)strlen(text) > c->cols - 1) text[c->cols - 1] = '\0';
}

/* A footnote explaining what was actually measured. Without it a bar chart is
   decoration: the reader cannot tell tokens from calls, or which window. */
void vw_footer(canvas_t *c, char *text)
{
    vw_footer_fit(c, text);
    canvas_puts(c, 1, c->rows - 1, text, PAL_DIM);
}

void vw_right_text(canvas_t *c, int row, int right_col, const char *s,
                       uint16_t colour)
{
    canvas_puts(c, right_col - (int)strlen(s), row, s, colour);
}

/* "18d 20h 19m", dropping the units that are zero from the left. */
void vw_duration(uint32_t secs, char *out, int size)
{
    unsigned d = secs / 86400u, h = (secs % 86400u) / 3600u, m = (secs % 3600u) / 60u;
    if (d) snprintf(out, size, "%ud %uh %um", d, h, m);
    else if (h) snprintf(out, size, "%uh %um", h, m);
    else snprintf(out, size, "%um", m);
}

/* "Aug 27" for a day count. */
void vw_day_name(int32_t days, char *out, int size)
{
    int y, m, d;
    timecalc_civil(days, &y, &m, &d);
    snprintf(out, size, "%s %d", timecalc_month_abbr(m), d);
}

/* A dim label followed by a coloured value, as one line. */
void vw_labelled(canvas_t *c, int col, int row, const char *label,
                     const char *value, uint16_t colour)
{
    canvas_puts(c, col, row, label, PAL_DIM);
    canvas_puts(c, col + (int)strlen(label), row, value, colour);
}

/* The heat grid's geometry: 53 week columns at their own pitch, wider than a
   text column, so the map is a mosaic of touching cells rather than a row of
   glyphs. Rows keep the text pitch so the weekday labels line up. */

/* One cell of the map at a pixel position. Empty days get a dot, so they
   still read as days rather than as gaps in the map. */
void vw_heat_cell(canvas_t *c, int x, int y, int level)
{
    int w = HEAT_PITCH_X - HEAT_GAP, h = c->cell_h - HEAT_GAP;
    if (level > 0) canvas_fill_rect(c, x, y, w, h, pal_heat(level));
    else canvas_fill_rect(c, x + w / 2 - 1, y + h / 2 - 1, 2, 2, pal_heat(0));
}

/* "$1,234" from a hundred dollars up, "$12.34" below. */
void vw_money(uint64_t cents, char *out, int size)
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

/* "1h 21m" or "14m" or "38s". */
void vw_age(uint32_t secs, char *out, int size)
{
    unsigned long v = secs;
    if (v >= 86400) snprintf(out, size, "%lud %luh", v / 86400, (v % 86400) / 3600);
    else if (v >= 3600) snprintf(out, size, "%luh %lum", v / 3600, (v % 3600) / 60);
    else if (v >= 60) snprintf(out, size, "%lum", v / 60);
    else snprintf(out, size, "%lus", v);
}

/* A step line of `n` daily values on a plot, values below zero skipped. Axis
   labels at 0, half and full of `top`; dates a fortnight apart beneath. */
void vw_step_plot(canvas_t *c, const int64_t *vals, int n, int64_t lo,
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
        vw_right_text(c, yy / c->cell_h, gutter / c->cell_w - 1, lab, PAL_DIM);
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
        vw_day_name(start_day + i, lab, sizeof lab);
        int col = (gutter + plot_w * i / n) / c->cell_w;
        if (col + (int)strlen(lab) <= c->cols) canvas_puts(c, col, date_row, lab, PAL_DIM);
    }
}

void vw_label_count(int64_t v, char *out, int size) { vw_human((uint64_t)v, out, size); }

/* The next 1, 2 or 5 times a power of ten at or above v, so an axis ends on
   a round number and its midpoint reads as one too. */
int64_t vw_nice_top(int64_t v)
{
    if (v <= 0) return 1;
    int64_t mag = 1;
    while (mag * 10 <= v) mag *= 10;
    if (v <= mag) return mag;
    if (v <= 2 * mag) return 2 * mag;
    if (v <= 5 * mag) return 5 * mag;
    return 10 * mag;
}

void vw_label_pct(int64_t v, char *out, int size) { snprintf(out, size, "%lld%%", (long long)v); }

void views_tools(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    const ud_tools_view_t *tl = &d->tools;
    char head[48], fresh[24];
    vw_freshness(d, now_us, fresh, sizeof fresh);
    if (tl->present) snprintf(head, sizeof head, "%d days   %s", tl->days, fresh);
    else snprintf(head, sizeof head, "%s", fresh);
    vw_title(c, "TOOLS", head);
    if (!tl->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    canvas_puts(c, 1, 1, "tool", PAL_DIM);
    canvas_puts(c, 17, 1, "share of calls", PAL_DIM);
    vw_right_text(c, 1, c->cols - 1, "calls", PAL_DIM);
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
        vw_right_text(c, row, c->cols - 1, buf, PAL_FG);
    }

    char line[128];
    snprintf(line, sizeof line, "%lu calls   %.0f per session   %.2f per message",
             (unsigned long)tl->calls,
             tl->sessions ? (double)tl->calls / tl->sessions : 0.0,
             tl->msgs ? (double)tl->calls / tl->msgs : 0.0);
    vw_footer_fit(c, line);
    canvas_puts(c, 1, 11, line, PAL_DIM);

    canvas_puts(c, 1, 12, "tool calls per day, last 60 days", PAL_DIM);
    int64_t vals[UD_MDAYS];
    int64_t top = 1;
    for (int i = 0; i < tl->len; i++) {
        vals[i] = (int64_t)tl->day[i];
        if (vals[i] > top) top = vals[i];
    }
    vw_step_plot(c, vals, tl->len, 0, vw_nice_top(top), tl->start, 13, 18,
              pal_heat(PAL_HEAT_STEPS), t, vw_label_count);

    snprintf(line, sizeof line,
             "counts tool_use blocks; older days from Claude Code's own cache");
    vw_footer(c, line);
}

/* "20 Aug 2026" for a day count, for records that may span years. */
void vw_full_date(int32_t days, char *out, int size)
{
    int y, m, d;
    timecalc_civil(days, &y, &m, &d);
    snprintf(out, size, "%d %s %d", d, timecalc_month_abbr(m), y);
}

void vw_label_secs(int64_t v, char *out, int size)
{
    if (v >= 3600) snprintf(out, size, "%lldh", (long long)(v / 3600));
    else if (v >= 60) snprintf(out, size, "%lldm", (long long)(v / 60));
    else snprintf(out, size, "%llds", (long long)v);
}

/* A filled marker beside a legend or card entry. */
void vw_dot(canvas_t *c, int col, int row, uint16_t colour)
{
    canvas_fill_rect(c, col * c->cell_w + 3, row * c->cell_h + c->cell_h / 2 - 3,
                     6, 6, colour);
}
