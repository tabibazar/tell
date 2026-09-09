#include "view_common.h"

#include <stdio.h>
#include <string.h>
#include "textwrap.h"

void views_story(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    (void)t;
    canvas_clear(c);
    const ud_story_view_t *st = &d->story;
    char head[72], fresh[24];
    vw_freshness(d, now_us, fresh, sizeof fresh);
    if (st->present && st->day > 0) {
        char when[24];
        vw_full_date(st->day, when, sizeof when);
        snprintf(head, sizeof head, "%s   %s", when, fresh);
    } else {
        snprintf(head, sizeof head, "%s", fresh);
    }
    vw_title(c, "TODAY'S STORY", head);
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
    vw_footer(c, note);
}
