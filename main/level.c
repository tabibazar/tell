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

/* One signed angle, printed to a tenth, with its sign always shown so the
   number does not jump sideways as it crosses zero. */
static void put_angle(canvas_t *c, int col, int row, const char *label,
                      float deg, uint16_t colour)
{
    char buf[24];
    snprintf(buf, sizeof buf, "%-5s %+5.1f", label, (double)deg);
    canvas_puts(c, col, row, buf, colour);
    degree_mark(c, col + (int)strlen(buf), row, colour);
}

void level_draw(canvas_t *c, float tilt_x_deg, float tilt_y_deg,
                float hold_s, float best_s, const char *note)
{
    canvas_clear(c);

    bool ok = level_is_true(tilt_x_deg, tilt_y_deg);
    uint16_t accent = ok ? PAL_A2 : PAL_A1;

    /* The dial sits on the left, square, as large as the short side allows.
       The readout takes the columns left over on the right. */
    int r = c->h / 2 - 18;
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

    /* The readout. Five rows is all the panel has, so every one earns its
       place: the two axes, how far off it is altogether, that same figure as
       a builder would want it, and which way the surface falls. */
    put_angle(c, text_col, 0, "X", tilt_x_deg, accent);
    put_angle(c, text_col, 1, "Y", tilt_y_deg, accent);

    float skew = sqrtf(tilt_x_deg * tilt_x_deg + tilt_y_deg * tilt_y_deg);
    char buf[24];
    snprintf(buf, sizeof buf, "SKEW %4.1f", (double)skew);
    canvas_puts(c, text_col, 2, buf, accent);
    degree_mark(c, text_col + (int)strlen(buf), 2, accent);

    if (ok) {
        /* While it is true the slope is zero and says nothing, so the two
           bottom rows become the clock instead: how long you have held it,
           and the longest anyone has. The bubble's ring already says TRUE. */
        snprintf(buf, sizeof buf, "HOLD %5.1f", (double)hold_s);
        canvas_puts(c, text_col, 3, buf, PAL_A2);
        snprintf(buf, sizeof buf, "BEST %5.1f", (double)best_s);
        canvas_puts(c, text_col, 4, buf, PAL_FG);
    } else {
        /* Millimetres per metre, which is how a slope is actually quoted on
           a building site, and far easier to act on than a fraction of a
           degree: a degree is about seventeen and a half of them. */
        snprintf(buf, sizeof buf, "%5.1f mm/m",
                 (double)(tanf(skew * (float)M_PI / 180.0f) * 1000.0f));
        canvas_puts(c, text_col, 3, buf, PAL_DIM);

        /* Which way it falls, in the screen's own terms so it needs no
           thinking about: the low edge is the one the bubble runs away from.
           U and D are the top and bottom of the panel as you are looking at
           it, L and R its sides. */
        char dir[4];
        int n = 0;
        if (tilt_y_deg >  LEVEL_TOLERANCE_DEG) dir[n++] = 'D';
        if (tilt_y_deg < -LEVEL_TOLERANCE_DEG) dir[n++] = 'U';
        if (tilt_x_deg >  LEVEL_TOLERANCE_DEG) dir[n++] = 'R';
        if (tilt_x_deg < -LEVEL_TOLERANCE_DEG) dir[n++] = 'L';
        dir[n] = '\0';
        snprintf(buf, sizeof buf, "LOW %s", dir);
        canvas_puts(c, text_col, 4, buf, PAL_A1);
    }

    /* A single letter for the state of the zero, in the corner, because the
       numbers alone cannot say whether they are absolute or relative to a
       surface someone chose. */
    if (note != NULL && note[0] != '\0')
        canvas_puts(c, c->cols - (int)strlen(note), c->rows - 1, note, PAL_DIM);
}
