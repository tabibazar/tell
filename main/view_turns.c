#include "view_common.h"

#include <stdio.h>
#include <string.h>

void views_turns(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    canvas_clear(c);
    const ud_turns_view_t *tn = &d->turns;
    char head[72], fresh[24];
    vw_freshness(d, now_us, fresh, sizeof fresh);
    if (tn->present) snprintf(head, sizeof head, "%d days   %s", tn->days, fresh);
    else snprintf(head, sizeof head, "%s", fresh);
    vw_title(c, "TURNS", head);
    if (!tn->present) {
        canvas_puts(c, 1, 2, "no data yet -- run tools/push-stats.sh", PAL_DIM);
        return;
    }

    const uint16_t hi = pal_heat(PAL_HEAT_STEPS);
    const int left = 1, right = c->cols / 2 + 1;
    char a[24], buf[64];
    vw_age(tn->first_med, a, sizeof a);
    vw_labelled(c, left, 2, "First reply, typically: ", a, hi);
    vw_age(tn->turn_med, a, sizeof a);
    vw_labelled(c, right, 2, "A turn, typically: ", a, hi);
    vw_age(tn->first_p90, a, sizeof a);
    vw_labelled(c, left, 3, "9 in 10 replies within: ", a, PAL_FG);
    vw_age(tn->turn_p90, a, sizeof a);
    vw_labelled(c, right, 3, "9 in 10 turns within: ", a, PAL_FG);

    vw_age(tn->longest, a, sizeof a);
    vw_labelled(c, left, 5, "Longest turn: ", a, hi);
    if (tn->longest_day > 0) {
        char when[24];
        vw_full_date(tn->longest_day, when, sizeof when);
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
    vw_step_plot(c, tn->day, tn->len, 0, nice, tn->start, 12, 18, hi, t, vw_label_secs);

    char note[96];
    snprintf(note, sizeof note, "typical = median; from the transcripts on this Mac");
    vw_footer(c, note);
}
