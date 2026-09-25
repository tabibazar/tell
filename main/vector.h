#ifndef VECTOR_H
#define VECTOR_H

#include "canvas.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Anti-aliased vector drawing onto a canvas, for watch's analog face: hands,
 * indices, sub-dial rings and arcs that have to look smooth on a 240x280
 * panel, where a jagged one-pixel staircase on a slowly turning hand is the
 * first thing the eye finds.
 *
 * Coordinates are floats in panel pixels, and pixel (i, j) is the unit square
 * [i, i+1) x [j, j+1), its centre at (i + 0.5, j + 0.5). So a disc centred at
 * (120, 140) sits exactly in the middle of a 240x280 panel, straddling four
 * pixels, and one centred on pixel (120, 140) is at (120.5, 140.5). A
 * one-pixel-wide horizontal line at y = 20.5 fills row 20 exactly; at y = 21
 * it lights rows 20 and 21 half each.
 *
 * Each pixel is shaded by how much of its square the shape covers, worked out
 * from the distance between the pixel centre and the shape's edge -- or, for
 * polygons, from the exact area of the square the outline encloses -- rather
 * than by supersampling, which on the ESP32-S3 would cost many times the
 * pixels for a face that is redrawn every second. Only pixels near the shape
 * are visited, and only the ones on an edge pay for a square root. (The one
 * thing this is not accurate for is a disc, or a ring's hole, less than a
 * pixel across, which comes out brighter than its area; nothing on a face is
 * that small.)
 *
 * Everything is clipped to the canvas: no write lands outside fb[0 .. w*h),
 * whatever the coordinates. A NaN or infinite argument draws nothing. Lines
 * and polygons may reach anywhere in float range: they are clipped working
 * from the end nearest the canvas, so a hand drawn from the centre towards a
 * point 1e30 away comes out exactly as the same hand drawn to the panel's
 * edge. Circles (discs, rings, arcs) whose centre, radius or width is beyond
 * a million pixels are not drawn, since float cannot place their edge to
 * within a pixel out there anyway.
 *
 * Colours are RGB565. Nothing allocates. The polygon fill keeps its working
 * rows in static buffers, so these are not reentrant: draw from one task.
 */

/* The most vertices vec_polygon takes; a longer outline draws nothing. A
   leaf-shaped hand needs about a dozen. */
#define VEC_POLY_MAX 64

/* `src` laid over `dst` at `alpha` (0 keeps dst exactly, 255 gives src
   exactly), per channel and rounded. */
uint16_t vec_blend(uint16_t dst, uint16_t src, uint8_t alpha);

/*
 * The same in linear light. RGB565 codes are gamma-encoded (the panel's
 * response is close to a 2.2 power), so mixing the codes, as vec_blend does,
 * lays a colour over black at half coverage as a quarter of its light, not
 * half: a bright edge on a dark ground comes out too dark and a stroke looks
 * thinner than its outline. This decodes each channel to light, mixes, and
 * encodes back, rounding in the encoded scale. It is what aafont.c blends
 * its type with, so marks drawn this way have the same edges as the text
 * beside them.
 */
uint16_t vec_blend_linear(uint16_t dst, uint16_t src, uint8_t alpha);

/*
 * Whether the shapes below blend their edges in linear light (true) or by
 * vec_blend (false, the default -- the watch face was judged that way and
 * stays exactly as it was). Returns the setting it replaces, so a caller can
 * put it back. A setting, not a parameter, as the drawing is single-task.
 */
bool vec_linear_light(bool on);

/* A straight stroke `width` pixels wide from (x0,y0) to (x1,y1), with round
   caps: every point within width/2 of the segment. A zero-length line is a
   dot of that diameter. Widths below one pixel fade rather than vanish, so a
   hairline stays visible at the brightness its width deserves. */
void vec_line(canvas_t *c, float x0, float y0, float x1, float y1,
              float width, uint16_t colour);

/* A filled polygon of n points, xy = {x0, y0, x1, y1, ...}, closed back to the
   first. Concave outlines are fine. Nonzero winding: a region wound twice is
   filled once, and a loop wound the other way inside another cuts a hole.
   For a simple outline, one that does not cross itself, that is the same fill
   either rule would give. Needs 3 <= n <= VEC_POLY_MAX. */
void vec_polygon(canvas_t *c, const float *xy, int n, uint16_t colour);

/* A filled disc of radius r. */
void vec_disc(canvas_t *c, float cx, float cy, float r, uint16_t colour);

/* A circle outline `width` wide, centred on radius r: it covers r - width/2
   to r + width/2. */
void vec_ring(canvas_t *c, float cx, float cy, float r, float width,
              uint16_t colour);

/* Part of a ring, from angle a0 to a1 in radians, where 0 is 12 o'clock and
   positive runs clockwise on the panel, the way a watch hand turns: pi/2 is
   3 o'clock. The order of a0 and a1 does not matter; a span of 2*pi or more
   is the whole ring. Round caps, each a half-disc of the stroke's width, so
   the arc reaches width/2 past each angle. */
void vec_arc(canvas_t *c, float cx, float cy, float r, float width,
             float a0, float a1, uint16_t colour);

/* As vec_arc, but with butt caps: the ends are cut square along the radius at
   exactly a0 and a1, as a gauge scale or a power-reserve track wants. */
void vec_arc_butt(canvas_t *c, float cx, float cy, float r, float width,
                  float a0, float a1, uint16_t colour);

#endif /* VECTOR_H */
