#include "view_common.h"

#include <stdio.h>
#include <string.h>

static const char *const DOW_SHORT[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
static const char *const DOW_LONG[7] = {
    "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"
};

/* The rhythm grid: 24 hour columns at their own pitch across the width, and
   seven weekday rows taller than a text row so the map fills the top half. */
#define RHY_PITCH_X 31
#define RHY_PITCH_Y 36
#define RHY_GAP 1

void views_rhythm(canvas_t *c, const ud_view_t *d, float t, int64_t now_us)
{
    (void)t;
    canvas_clear(c);
    const ud_rhythm_view_t *r = &d->rhythm;
    char head[48];
    if (r->present) {
        snprintf(head, sizeof head, "%llu messages over %d days",
                 (unsigned long long)r->msgs, r->days);
        vw_title(c, "WHEN YOU WORK", head);
    } else {
        vw_title(c, "WHEN YOU WORK", NULL);
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
        vw_heat_cell(c, lx, ly, l);
    canvas_puts_px(c, lx + c->cell_w, ly, "More", PAL_DIM);

    const uint16_t hi = pal_heat(PAL_HEAT_STEPS);
    const int left = 1, right = c->cols / 2 + 1;
    int row = (ly + c->cell_h) / c->cell_h + 1;
    char buf[40];
    snprintf(buf, sizeof buf, "%02d:00-%02d:00", r->busiest_hour, (r->busiest_hour + 1) % 24);
    vw_labelled(c, left, row, "Busiest hour: ", buf, hi);
    vw_labelled(c, right, row, "Busiest day: ", DOW_LONG[r->busiest_dow], hi);
    row++;
    snprintf(buf, sizeof buf, "%d%%", r->night_pct);
    vw_labelled(c, left, row, "Nights (22-06): ", buf, hi);
    snprintf(buf, sizeof buf, "%d%%", r->weekend_pct);
    vw_labelled(c, right, row, "Weekends: ", buf, hi);
    row++;
    snprintf(buf, sizeof buf, "%s %02d:00, %lu messages", DOW_SHORT[r->peak_dow],
             r->peak_hour, (unsigned long)r->cell[r->peak_dow * 24 + r->peak_hour]);
    vw_labelled(c, left, row, "Peak hour: ", buf, hi);

    char note[96];
    snprintf(note, sizeof note,
             "cell = messages in that weekday hour, shaded by quartile");
    vw_footer(c, note);
}
