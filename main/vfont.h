#ifndef VFONT_H
#define VFONT_H

#include "canvas.h"

#include <stdint.h>

/*
 * A small stroke font for watch's dial: the day names round a sub-dial, the
 * date numerals, the moon page's lines. The 12x24 bitmap font cannot go below
 * its cell, and on a sub-dial 52 px across a label has to be five pixels
 * tall, set on a curve, and anti-aliased like everything else on the face.
 *
 * Capitals, digits and a little punctuation only; lower case comes out as
 * capitals, which is how a dial is lettered anyway. Anything else advances
 * like a space and draws nothing.
 *
 * Each glyph is a handful of centre lines (straight runs and elliptical
 * arcs) on a grid ten units tall, stroked at any weight. A pixel is shaded by
 * its distance to the nearest of the glyph's centre lines, all of them at
 * once, rather than by drawing each run as its own anti-aliased line: where
 * two runs meet, or an arc is made of many short ones, a pixel on the edge
 * would otherwise be blended in twice and every joint and curve would come
 * out heavier than a straight stroke. No allocation; the glyph being drawn
 * is worked in one static buffer, so this is not reentrant: draw from one
 * task, as for vector.c.
 */

#define VFONT_LEFT   (-1)
#define VFONT_CENTRE 0
#define VFONT_RIGHT  1

typedef struct {
    float size;      /* cap height in pixels */
    float weight;    /* stroke width in pixels; under 1 fades rather than
                        vanishing, as vec_line does */
    float tracking;  /* extra pixels between letters, for spaced capitals */
} vfont_style_t;

/* Width in pixels of `s` set in `st`, from the left of its first letter's
   centre lines to the right of its last; the stroke adds half a weight
   either side. */
float vfont_width(const vfont_style_t *st, const char *s);

/*
 * Draws `s` in `colour`. (x, y) is the anchor: vertically the middle of the
 * capitals, horizontally the left end, the middle or the right end of the
 * line by `align` (VFONT_LEFT, VFONT_CENTRE, VFONT_RIGHT). `angle` turns the
 * whole line about the anchor, in radians, clockwise on the panel as a watch
 * hand turns; 0 is upright. Clipped to the canvas; NaN or infinite
 * arguments draw nothing.
 */
void vfont_draw(canvas_t *c, const vfont_style_t *st, float x, float y,
                float angle, int align, uint16_t colour, const char *s);

#endif /* VFONT_H */
