#ifndef VIEW_COMMON_H
#define VIEW_COMMON_H

/*
 * What every page shares: the title bar and footer, number and date
 * formatting, the heat cell, the step plot. One page per view_*.c file
 * uses these; nothing here knows which page it is drawing.
 */
#include "canvas.h"
#include "palette.h"
#include "timecalc.h"
#include "usagedata.h"
#include "views.h"

#include <stdbool.h>
#include <stdint.h>

#define HEAT_PITCH_X 14
#define HEAT_GAP     1

void vw_human(uint64_t n, char *out, int size);
void vw_freshness(const ud_view_t *d, int64_t now_us, char *out, int size);
void vw_title(canvas_t *c, const char *left, const char *right);
void vw_footer_fit(canvas_t *c, char *text);
void vw_footer(canvas_t *c, char *text);
void vw_right_text(canvas_t *c, int row, int right_col, const char *s, uint16_t colour);
void vw_duration(uint32_t secs, char *out, int size);
void vw_day_name(int32_t days, char *out, int size);
void vw_labelled(canvas_t *c, int col, int row, const char *label, const char *value, uint16_t colour);
void vw_heat_cell(canvas_t *c, int x, int y, int level);
void vw_money(uint64_t cents, char *out, int size);
void vw_age(uint32_t secs, char *out, int size);
void vw_step_plot(canvas_t *c, const int64_t *vals, int n, int64_t lo, int64_t top, int32_t start_day, int plot_top_row, int date_row, uint16_t colour, float t, void (*label)(int64_t, char *, int));
int64_t vw_nice_top(int64_t v);
void vw_full_date(int32_t days, char *out, int size);
void vw_label_secs(int64_t v, char *out, int size);
void vw_dot(canvas_t *c, int col, int row, uint16_t colour);
void vw_label_count(int64_t v, char *out, int size);
void vw_label_pct(int64_t v, char *out, int size);

/* What the title bars show on the right: the local time with its zone, and
   UTC. main sets this every tick from the board's clock and the offset the
   Mac's clock payload carries; until both are known the bars show nothing
   there. */
void vw_set_clock(bool known, uint32_t local_secs, int utc_offset_min,
                  const char *tz);

/* The strip itself, or an empty string when unknown; for tests and layout. */
const char *vw_clock_strip(void);

/* The MENU tab at the top left of a page, and whether a tap landed on it.
   vw_title draws it; the clock and message pages, which have no title bar,
   draw it themselves. The hit area is bigger than the tab (fingertips). */
#define VW_TAB_COLS 6
void vw_menu_tab(canvas_t *c);
bool vw_menu_tab_hit(canvas_t *c, int x, int y);

#endif /* VIEW_COMMON_H */
