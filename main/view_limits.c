#include "view_common.h"

#include <stdio.h>
#include <string.h>

/* The countdown, to the second while it is short enough for a second to
   matter, and to the minute once it is days away: a weekly reset ticking its
   seconds down is noise, and the seconds are three characters that could be
   telling you the day instead. */
static void countdown(int64_t secs, char *out, int size)
{
    if (secs <= 0) { snprintf(out, size, "due"); return; }
    unsigned d = (unsigned)(secs / 86400), rest = (unsigned)(secs % 86400);
    unsigned h = rest / 3600, m = (rest % 3600) / 60, s = rest % 60;
    if (d) snprintf(out, size, "%ud %02u:%02u", d, h, m);
    else if (h) snprintf(out, size, "%u:%02u:%02u", h, m, s);
    else snprintf(out, size, "%02u:%02u", m, s);
}

/* Amber as it fills, vermillion once it is spent. The severity the account reports
   wins where it is stronger, so a limit Claude Code calls critical looks
   critical here even if its own percentage reads mildly. */
static uint16_t bar_colour(const ud_limit_t *r)
{
    if (r->severity >= 2 || r->percent >= 100) return PAL_A5;
    if (r->severity == 1 || r->percent >= 80) return pal_heat(PAL_HEAT_STEPS);
    return pal_heat(PAL_HEAT_STEPS - 1);
}

/* "Session", "Week, all models", "Week, Fable". */
static void row_label(const ud_limit_t *r, char *out, int size)
{
    if (strcmp(r->kind, "session") == 0) snprintf(out, size, "SESSION");
    else if (r->scope[0] != '\0') snprintf(out, size, "WEEK, %s", r->scope);
    else snprintf(out, size, "WEEK, ALL MODELS");
    for (char *p = out; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
}

void views_limits(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    const ud_limits_view_t *l = &d->limits;
    char fresh[24];
    vw_freshness(d, now_us, fresh, sizeof fresh);
    vw_title(c, "LIMITS", fresh);

    if (!l->present || l->count == 0) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-now.sh", PAL_DIM);
        return;
    }

    /* How long the numbers have been sitting here. Every countdown on the
       page is its sent value less this. */
    int64_t aged = (now_us - l->sent_us) / 1000000;
    if (aged < 0) aged = 0;

    const int left = 2;
    const int bar_x = left * c->cell_w;
    const int bar_w = (c->cols - 4) * c->cell_w;
    const int block = 4;                  /* rows per limit: label, bar, gap */

    for (int i = 0; i < l->count && i < 4; i++) {
        const ud_limit_t *r = &l->rows[i];
        int row = 2 + i * block;
        char label[32], pct[8], left_s[24];

        row_label(r, label, sizeof label);
        canvas_puts(c, left, row, label, r->active ? PAL_FG : PAL_DIM);

        snprintf(pct, sizeof pct, "%u%%", (unsigned)r->percent);
        vw_right_text(c, row, left + 24, pct, PAL_FG);

        countdown((int64_t)r->reset_secs - aged, left_s, sizeof left_s);
        char when[40];
        snprintf(when, sizeof when, "resets in %s", left_s);
        vw_right_text(c, row, c->cols - 1, when, PAL_DIM);

        /* The trough first, so an empty limit is still a visible measure of
           how much room there is. */
        int y = (row + 1) * c->cell_h + 2;
        int h = c->cell_h - 6;
        canvas_fill_rect(c, bar_x, y, bar_w, h, 0x2124);
        int fill = (int)(bar_w * (r->percent / 100.0) * t);
        if (fill > bar_w) fill = bar_w;
        if (fill < 2 && r->percent > 0 && t >= 1.0f) fill = 2;
        canvas_fill_rect(c, bar_x, y, fill, h, bar_colour(r));
    }

    /* Extra usage, when the account has it turned on: it is what carries on
       paying once the weekly allowance is gone, so it belongs beside them. */
    if (l->have_credits) {
        int row = 2 + (l->count < 4 ? l->count : 4) * block;
        if (row <= c->rows - 4) {
            char money[48], pct[8];
            snprintf(money, sizeof money, "EXTRA USAGE   %u.%02u of %u.%02u %s",
                     (unsigned)(l->credit_used / 100), (unsigned)(l->credit_used % 100),
                     (unsigned)(l->credit_limit / 100), (unsigned)(l->credit_limit % 100),
                     l->currency);
            canvas_puts(c, left, row, money, PAL_DIM);
            snprintf(pct, sizeof pct, "%u%%", (unsigned)l->credit_pct);
            vw_right_text(c, row, c->cols - 1, pct, PAL_FG);

            int y = (row + 1) * c->cell_h + 2;
            int h = c->cell_h - 6;
            canvas_fill_rect(c, bar_x, y, bar_w, h, 0x2124);
            int fill = (int)(bar_w * (l->credit_pct / 100.0) * t);
            if (fill > bar_w) fill = bar_w;
            canvas_fill_rect(c, bar_x, y, fill, h,
                             l->credit_pct >= 90 ? PAL_A5 : PAL_A0);
        }
    }

    char note[96];
    snprintf(note, sizeof note,
             "the same figures /usage reports; countdowns run on the board");
    vw_footer(c, note);
}
