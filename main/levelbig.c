#include "levelbig.h"

#include "level.h"
#include "palette.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Same three rings as level.c's dial: two for scale, the outer one the
   full-scale rim. */
static const float RINGS[] = { 1.0f, 2.5f, LEVEL_FULL_SCALE_DEG };
#define RING_COUNT ((int)(sizeof RINGS / sizeof RINGS[0]))

/* Bigger than level.c's three-pixel dot: this panel has roughly eleven times
   the linear pixels to spend on a 280px dial, and a dot sized for the
   Feather's would be lost on it. */
#define BUBBLE_R 10

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Draws `s` at a scale of its own, centred horizontally, top edge at `y`.
   The canvas's configured scale (set once, at init, for the whole board) is
   not the scale every line here wants -- the timer should read across the
   room and the footer should not swallow the panel -- so this borrows
   canvas_puts_px's glyph engine at a scale of its own for one call and puts
   the canvas back the way it found it. cell_w/cell_h scale linearly with
   scale (canvas_init sets them to the font cell times it), so the font's
   native size falls out of the canvas's own current values without this
   file needing its own copy of it. */
static void big_puts(canvas_t *c, const char *s, int want_scale, int y,
                     uint16_t colour)
{
    int base_scale = c->scale > 0 ? c->scale : 1;
    int font_w = c->cell_w / base_scale;
    int font_h = c->cell_h / base_scale;

    int save_scale = c->scale, save_cw = c->cell_w, save_ch = c->cell_h;
    c->scale = want_scale;
    c->cell_w = font_w * want_scale;
    c->cell_h = font_h * want_scale;

    int len = (int)strlen(s);
    int x = (c->w - len * c->cell_w) / 2;
    if (x < 0) x = 0;
    canvas_puts_px(c, x, y, s, colour);

    c->scale = save_scale;
    c->cell_w = save_cw;
    c->cell_h = save_ch;
}

/* As big_puts, but always at the canvas's own configured scale -- for the
   two smaller readout lines under the timer. */
static void line_puts(canvas_t *c, const char *s, int y, uint16_t colour)
{
    int len = (int)strlen(s);
    int x = (c->w - len * c->cell_w) / 2;
    if (x < 0) x = 0;
    canvas_puts_px(c, x, y, s, colour);
}

void levelbig_draw(canvas_t *c, float tilt_x_deg, float tilt_y_deg,
                   float hold_s, float last_s, float prev_s, float best_s,
                   bool armed)
{
    canvas_clear(c);

    bool ok = level_is_true(tilt_x_deg, tilt_y_deg);

    /* The bullseye, centred on the panel and as large as the width allows,
       with the readout stacked in the taller half left under it. */
    int cx = c->w / 2;
    int cy = c->w / 2 + 40;
    int r  = c->w / 2 - 20;

    for (int i = 0; i < RING_COUNT; i++) {
        int rr = (int)(RINGS[i] / LEVEL_FULL_SCALE_DEG * (float)r);
        canvas_circle(c, cx, cy, rr, PAL_A0);
    }

    /* The tolerance ring: inside this the board counts as true and the clock
       runs. Drawn thicker while it is true, so the state is readable without
       relying on a colour change alone. */
    int tol_r = (int)(LEVEL_TOLERANCE_DEG / LEVEL_FULL_SCALE_DEG * (float)r);
    if (tol_r < BUBBLE_R + 4) tol_r = BUBBLE_R + 4;
    canvas_circle(c, cx, cy, tol_r, PAL_A1);
    if (ok) canvas_circle(c, cx, cy, tol_r + 1, PAL_A1);

    /* Centre crosshair: where the bubble sits when the board is true. */
    canvas_fill_rect(c, cx - 14, cy, 29, 2, PAL_DIM);
    canvas_fill_rect(c, cx, cy - 14, 2, 29, PAL_DIM);

    /* The bubble. Matches level.c's convention exactly: it floats to the
       HIGH side, against gravity, so a positive tilt_x_deg (that edge down)
       moves it toward -x, not +x -- level.c's level_draw computes the same
       negation (`sx = clampf(-tilt_x_deg, ...)`) for the same reason. */
    float sx = clampf(-tilt_x_deg, -LEVEL_FULL_SCALE_DEG, LEVEL_FULL_SCALE_DEG);
    float sy = clampf(-tilt_y_deg, -LEVEL_FULL_SCALE_DEG, LEVEL_FULL_SCALE_DEG);
    int bx = cx + (int)(sx / LEVEL_FULL_SCALE_DEG * (float)r);
    int by = cy + (int)(sy / LEVEL_FULL_SCALE_DEG * (float)r);
    canvas_disc(c, bx, by, BUBBLE_R, PAL_A1);

    /* The readout, stacked under the dial: the earned-clock timer large
       (or a hint to pick the board up, when no run has been armed yet),
       the raw degrees, and the last/prev/best runs. */
    char buf[32];
    int y = cy + r + 12;
    if (armed) {
        snprintf(buf, sizeof buf, "%.1fs", (double)hold_s);
        big_puts(c, buf, 2, y, ok ? PAL_A1 : PAL_FG);
    } else {
        big_puts(c, "TILT TO START", 1, y + 12, PAL_A1);
    }

    snprintf(buf, sizeof buf, "R %+.1f  P %+.1f",
             (double)tilt_x_deg, (double)tilt_y_deg);
    line_puts(c, buf, y + 54, PAL_FG);

    snprintf(buf, sizeof buf, "L %.1f  P %.1f  B %.1f",
             (double)last_s, (double)prev_s, (double)best_s);
    line_puts(c, buf, y + 82, PAL_DIM);
}
