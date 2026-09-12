#include "view_common.h"

#include <stdio.h>
#include <string.h>

/*
 * All-time usage on a small panel.
 *
 * The big panel has a page each for models, for cost and for the year, and
 * room to draw them. Twenty-six columns by seven will not hold any of those,
 * but it will hold the four numbers you actually want when you glance at a
 * board on a desk: how many tokens have gone through, what they would have
 * cost on the API, how long that has been accumulating, and which three
 * models did most of it.
 *
 * "Tokens" means output plus cache-read, which is what the By model page's
 * bars measure. Two pages that both say "total" must count the same things.
 */

/* The three sections arrive in separate payloads, so any of them may be
   missing while the others are not. Each line says so on its own rather than
   the page refusing to draw. */
static void line(canvas_t *c, int row, const char *label, const char *value,
                 uint16_t colour)
{
    canvas_puts(c, 0, row, label, PAL_DIM);
    vw_right_text(c, row, c->cols - 1, value, colour);
}

void views_usage(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    (void)t; (void)now_us;

    canvas_clear(c);
    canvas_fill_rect(c, 0, 0, c->w, c->cell_h, PAL_TITLE_BG);
    canvas_puts(c, 0, 0, "USAGE", PAL_FG);
    /* The full strip carries both zones and will not fit beside a title on
       twenty-six columns, so this panel gets the local time alone. */
    const char *strip = vw_clock_short();
    if (strip[0] != '\0') canvas_puts(c, c->cols - (int)strlen(strip), 0, strip, PAL_FG);

    uint64_t grand = 0;
    for (int i = 0; i < d->model_count; i++)
        grand += d->models[i].cread + d->models[i].out;

    char value[24];
    if (grand > 0) {
        vw_human(grand, value, sizeof value);
        strncat(value, " tok", sizeof value - strlen(value) - 1);
    } else {
        snprintf(value, sizeof value, "--");
    }
    line(c, 1, "all time", value, PAL_FG);

    if (d->cost.present && d->cost.total > 0) vw_money(d->cost.total, value, sizeof value);
    else snprintf(value, sizeof value, "--");
    line(c, 2, "on the API", value, PAL_A1);

    /* How long it has been running, and how much of that was worked: a span
       with no active days in it would flatter the total. */
    if (d->year.present && d->year.span_days > 0)
        snprintf(value, sizeof value, "%d/%d days",
                 d->year.active_days, d->year.span_days);
    else
        snprintf(value, sizeof value, "--");
    line(c, 3, "active", value, PAL_FG);

    /*
     * The three biggest models, each with its share. Ranked here rather than
     * trusted to arrive ranked, because the merge across machines sums them
     * and nothing re-sorts afterwards.
     */
    int order[3] = { -1, -1, -1 };
    for (int i = 0; i < d->model_count; i++) {
        uint64_t mine = d->models[i].cread + d->models[i].out;
        for (int slot = 0; slot < 3; slot++) {
            if (order[slot] < 0) { order[slot] = i; break; }
            uint64_t theirs = d->models[order[slot]].cread + d->models[order[slot]].out;
            if (mine > theirs) {
                for (int k = 2; k > slot; k--) order[k] = order[k - 1];
                order[slot] = i;
                break;
            }
        }
    }

    for (int slot = 0; slot < 3; slot++) {
        int row = 4 + slot;
        if (row >= c->rows || order[slot] < 0) break;
        const ud_model_t *m = &d->models[order[slot]];
        uint64_t mine = m->cread + m->out;

        char name[20];
        snprintf(name, sizeof name, "%s", m->name);
        canvas_puts(c, 0, row, name, PAL_FG);

        vw_human(mine, value, sizeof value);
        if (grand > 0) {
            char share[40];
            snprintf(share, sizeof share, "%s %u%%", value,
                     (unsigned)(mine * 100 / grand));
            vw_right_text(c, row, c->cols - 1, share, PAL_DIM);
        } else {
            vw_right_text(c, row, c->cols - 1, value, PAL_DIM);
        }
    }
}
