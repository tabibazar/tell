#include "level.h"

#include "palette.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Rings, in degrees. The outermost is the full scale. */
static const int RINGS[] = { 5, 10, 15 };
#define RING_COUNT ((int)(sizeof RINGS / sizeof RINGS[0]))

#define BUBBLE_R 7

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

bool level_is_true(float tilt_x_deg, float tilt_y_deg)
{
    return fabsf(tilt_x_deg) <= LEVEL_TOLERANCE_DEG
        && fabsf(tilt_y_deg) <= LEVEL_TOLERANCE_DEG;
}

/* The degree sign, which the font does not have: a small ring, drawn at the
   top of the character cell that would have held it. */
static void degree_mark(canvas_t *c, int col, int row, uint16_t colour)
{
    int x = col * c->cell_w + c->cell_w / 2;
    int y = row * c->cell_h + c->cell_h / 3;
    canvas_circle(c, x, y, 2, colour);
}

void level_draw(canvas_t *c, float tilt_x_deg, float tilt_y_deg,
                float hold_s, float last_s, float best_s)
{
    canvas_clear(c);

    bool ok = level_is_true(tilt_x_deg, tilt_y_deg);
    uint16_t accent = ok ? PAL_A2 : PAL_A1;

    /* The dial sits on the left, square, as large as the short side allows.
       The readout takes the columns left over on the right. */
    int r = c->h / 2 - 12;
    int cx = r + 8;
    int cy = c->h / 2;
    int text_col = (cx + r + 8) / c->cell_w + 1;

    for (int i = 0; i < RING_COUNT; i++) {
        int rr = (int)((float)RINGS[i] / LEVEL_FULL_SCALE_DEG * (float)r);
        canvas_circle(c, cx, cy, rr, i == RING_COUNT - 1 ? PAL_FG : PAL_DIM);
    }

    /* Crosshair: this is where the bubble sits when the board is true, so it
       is drawn in the accent colour and the bubble can be read against it. */
    canvas_fill_rect(c, cx - r, cy, 2 * r + 1, 1, PAL_DIM);
    canvas_fill_rect(c, cx, cy - r, 1, 2 * r + 1, PAL_DIM);

    /* A tick on each axis at every ring, so the scale can be read without
       counting circles. */
    for (int i = 0; i < RING_COUNT; i++) {
        int rr = (int)((float)RINGS[i] / LEVEL_FULL_SCALE_DEG * (float)r);
        canvas_fill_rect(c, cx + rr, cy - 3, 1, 7, PAL_FG);
        canvas_fill_rect(c, cx - rr, cy - 3, 1, 7, PAL_FG);
        canvas_fill_rect(c, cx - 3, cy + rr, 7, 1, PAL_FG);
        canvas_fill_rect(c, cx - 3, cy - rr, 7, 1, PAL_FG);
    }

    /* The bubble, stopped at the rim rather than sliding off the dial.
       It floats to the HIGH side, which is the whole point of a bubble and
       the opposite of where gravity points: tip the right edge down and the
       bubble goes left, exactly as it does in a real level. Drawn the other
       way round it reads upside down, because everyone has used one. */
    float sx = clampf(-tilt_x_deg, -LEVEL_FULL_SCALE_DEG, LEVEL_FULL_SCALE_DEG);
    float sy = clampf(-tilt_y_deg, -LEVEL_FULL_SCALE_DEG, LEVEL_FULL_SCALE_DEG);
    int bx = cx + (int)(sx / LEVEL_FULL_SCALE_DEG * (float)r);
    int by = cy + (int)(sy / LEVEL_FULL_SCALE_DEG * (float)r);
    canvas_disc(c, bx, by, BUBBLE_R, accent);
    /* A ring around it when it is true: the state must be readable without
       relying on the colour having changed. */
    if (ok) canvas_circle(c, bx, by, BUBBLE_R + 3, PAL_FG);

    /* The readout: how far off it is, and the three clocks, straight down the
       column. The two axes used to be here and are not any more -- they were
       the dial repeated in words, and reading a pair of signed numbers to
       learn what a picture was already telling you is work. */
    char buf[24];
    float skew = sqrtf(tilt_x_deg * tilt_x_deg + tilt_y_deg * tilt_y_deg);
    snprintf(buf, sizeof buf, "SKEW %4.1f", (double)skew);
    canvas_puts(c, text_col, 0, buf, accent);
    degree_mark(c, text_col + (int)strlen(buf), 0, accent);

    /* Running only while it is true, and lit while it runs, so the row that
       is moving is obvious at a glance. */
    snprintf(buf, sizeof buf, "HOLD %4.1f", (double)hold_s);
    canvas_puts(c, text_col, 1, buf, ok ? PAL_A2 : PAL_DIM);

    /* The last finished run, and the best there has ever been. Both outlive
       the power, so whoever picks the board up next has a mark to beat. */
    snprintf(buf, sizeof buf, "LAST %4.1f", (double)last_s);
    canvas_puts(c, text_col, 2, buf, PAL_FG);

    snprintf(buf, sizeof buf, "BEST %4.1f", (double)best_s);
    /* Lit while the run in progress has passed it, so a record announces
       itself rather than having to be worked out from two numbers. */
    bool record = hold_s > best_s && hold_s > 0.0f;
    canvas_puts(c, text_col, 3, buf, record ? PAL_A4 : PAL_DIM);

}
