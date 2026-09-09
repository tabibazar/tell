#include "view_common.h"

#include <stdio.h>
#include <string.h>

void views_today(canvas_t *c, const usagedata_t *d)
{
    canvas_clear(c);
    vw_title(c, "TODAY", d->date[0] ? d->date : NULL);

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
