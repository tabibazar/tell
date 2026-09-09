#include "view_common.h"

#include <stdio.h>
#include <string.h>
#include "pages.h"
#include "settings.h"

/* Settings layout: each setting takes a block of four text rows -- a label,
   two rows of buttons, a gap -- so three fit with room for a footer. Buttons
   are drawn two rows tall and hit-tested over three: one row is 24px, about
   4mm on this panel, and taps that missed it used to advance the page. */
/* Settings layout: each setting is a block of two text rows -- the label on
   the left, its buttons from SET_BTN_COL -- with one row of gap, so five fit
   above the footer. Buttons are two rows tall and hit-tested over three: one
   row is 24px, about 4mm on this panel, and taps that missed it used to
   advance the page. */
#define SET_TOP_ROW     2
#define SET_BLOCK_ROWS  3
#define SET_BUTTON_ROWS 2
#define SET_HIT_ROWS    3
#define SET_LEFT_COL    1
#define SET_BTN_COL     21
#define SET_GAP_COLS    1

/* A button's text column and width in columns; the text sits inside one
   column of padding each side. */
static void set_button_span(int row, int choice, int *col, int *cols)
{
    const settings_row_t *r = settings_row(row);
    int at = SET_BTN_COL;
    for (int i = 0; i < choice; i++)
        at += (int)strlen(r->choices[i]) + 2 + SET_GAP_COLS;
    *col = at;
    *cols = (int)strlen(r->choices[choice]) + 2;
}

void views_settings(canvas_t *c, const settings_t *s)
{
    canvas_clear(c);
    vw_title(c, "SETTINGS", NULL);

    for (int row = 0; row < SETTINGS_ROWS; row++) {
        const settings_row_t *r = settings_row(row);
        int top_row = SET_TOP_ROW + row * SET_BLOCK_ROWS;
        int y = top_row * c->cell_h;
        int h = SET_BUTTON_ROWS * c->cell_h;
        /* The label sits level with the middle of its buttons. */
        canvas_puts_px(c, SET_LEFT_COL * c->cell_w, y + (h - c->cell_h) / 2, r->label, PAL_DIM);

        int chosen = settings_choice(s, row);
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
    vw_footer(c, note);
}

bool views_settings_hit(canvas_t *c, int x, int y, int *row, int *choice)
{
    for (int r = 0; r < SETTINGS_ROWS; r++) {
        int top = (SET_TOP_ROW + r * SET_BLOCK_ROWS) * c->cell_h;
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
