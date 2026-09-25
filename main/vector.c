#include "vector.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * How the edges are shaded.
 *
 * Each pixel's coverage is the overlap of its square with the shape. For
 * strokes and circles that comes from one number, the distance from the
 * pixel's centre to the shape's centre line or edge: a pixel is taken to be a
 * unit interval across the edge, and the part of that interval inside the
 * shape is the coverage. That is exact where an edge crosses the pixel
 * square-on and close at any other angle. It also keeps a stroke thinner than
 * a pixel at its true weight: a half-pixel hairline comes out at half
 * brightness rather than as a full pixel or as gaps.
 *
 * Polygons are done differently, because a leaf-shaped hand has a sharp tip
 * that a distance function would round off. Each edge adds its exact signed
 * area to a row of cells, and a running sum along the row gives every pixel's
 * coverage -- the way font rasterisers do it -- in one pass over the rows,
 * with no per-pixel search over the edges.
 *
 * Nothing is supersampled. On the S3 a disc costs a multiply-add or two per
 * pixel, and a square root only on the one-pixel band at its edge, and
 * usually not even there.
 */

#define VEC_PI 3.14159265358979f
#define VEC_TWO_PI 6.28318530717959f

/* Circles whose centre, radius or width is beyond this are not drawn: float
   has only a sixteenth of a pixel to spare at a million, and nothing on a
   watch face is within a thousand screens of it. */
#define FAR 1.0e6f

/* Line and polygon coordinates are pinned to this before any arithmetic, so
   that no difference of two of them can overflow to infinity and come back as
   NaN. Nothing that far out has a meaningful position anyway. */
#define HUGE_COORD 1.0e30f

/* From this radius up, an edge pixel's distance from a circle comes from its
   squared distance with no square root: (D^2 - R^2) / 2R is D - R plus
   (D - R)^2 / 2R, under a sixtieth of a pixel across the edge band. Smaller
   circles, where that error grows, pay for sqrtf. */
#define APPROX_R 8.0f

/* The widest run of columns the polygon fill works on at once. A wider
   polygon is filled in strips, so the row buffer stays small. */
#define STRIP 256

static inline float minf(float a, float b) { return a < b ? a : b; }
static inline float maxf(float a, float b) { return a > b ? a : b; }

static inline float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* floor(v) as an index, clamped to [lo, hi] while it is still a float, so a
   huge or NaN value never reaches the conversion to int, where it would be
   undefined behaviour. */
static int to_index(float v, int lo, int hi)
{
    v = floorf(v);
    if (!(v > (float)lo)) return lo;
    if (v >= (float)hi) return hi;
    return (int)v;
}

static bool canvas_ok(const canvas_t *c)
{
    return c != NULL && c->fb != NULL && c->w > 0 && c->h > 0;
}

static inline float pin(float v)
{
    return v < -HUGE_COORD ? -HUGE_COORD : (v > HUGE_COORD ? HUGE_COORD : v);
}

uint16_t vec_blend(uint16_t dst, uint16_t src, uint8_t alpha)
{
    /* Per channel, rounded: (dst*(255-a) + src*a + 127) / 255. At a = 0 that
       is dst and at 255 it is src, exactly, with no special cases. The
       compiler turns the constant division into a multiply. */
    uint32_t a = alpha, na = 255u - a;
    uint32_t r = (((uint32_t)dst >> 11) * na + ((uint32_t)src >> 11) * a
                  + 127u) / 255u;
    uint32_t g = ((((uint32_t)dst >> 5) & 0x3Fu) * na
                  + (((uint32_t)src >> 5) & 0x3Fu) * a + 127u) / 255u;
    uint32_t b = (((uint32_t)dst & 0x1Fu) * na + ((uint32_t)src & 0x1Fu) * a
                  + 127u) / 255u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Lays `colour` over one pixel by `cov`, the fraction of it the shape covers.
   Callers have already clipped (x, y) to the canvas. */
static inline void plot(canvas_t *c, int x, int y, float cov, uint16_t colour)
{
    if (!(cov > 0.0f)) return;              /* also refuses a NaN outright */
    int a = cov >= 1.0f ? 255 : (int)(cov * 255.0f + 0.5f);
    if (a <= 0) return;
    uint16_t *p = &c->fb[(size_t)y * (size_t)c->w + (size_t)x];
    *p = a >= 255 ? colour : vec_blend(*p, colour, (uint8_t)a);
}

/* The coverage of a pixel whose centre is `d` (>= 0) from the centre line of
   a band `hw` either side of it: the overlap of [d - 1/2, d + 1/2] with
   [-hw, hw]. */
static inline float band(float d, float hw)
{
    return clamp01(minf(0.5f, hw - d) + minf(0.5f, hw + d));
}

/* ---- circles, rings and arcs ------------------------------------------- */

enum { RING, ARC_ROUND, ARC_BUTT };

/*
 * One description serves the disc, the ring and both arcs: an annulus from
 * ri to ro, optionally cut to an angular span. A disc is the annulus with no
 * hole (ri <= 0).
 *
 * An arc is worked in a frame turned so that the middle of the arc points
 * straight up, and folded about that axis (x taken as |x|), so that the two
 * ends are mirror images and only one cap ever needs testing. In that frame
 * a point is inside the span exactly when it is no further round from the
 * axis than half the sweep -- no atan2 per pixel.
 */
typedef struct {
    int kind;
    float cx, cy;
    float ri, ro;           /* inner and outer edge; ri <= 0 means no hole */
    float out2;             /* (ro + 1/2)^2: from here out, nothing */
    float hole2;            /* (ri - 1/2)^2, or -1: from here in, nothing */
    float full_out2;        /* (ro - 1/2)^2, or -1: radially full within... */
    float full_in2;         /* ...and beyond (ri + 1/2)^2, or 0 */
    float ro2, inv2ro, ri2, inv2ri;
    bool ro_approx, ri_approx;
    /* arcs only */
    float r, hw;            /* centre-line radius, half the width */
    float cm, sm;           /* cos and sin of the angle of the arc's middle */
    float ch, sh;           /* cos and sin of half the sweep */
    float capx, capy;       /* centre of the round cap, in the folded frame */
    float cap_out2, cap_full2;
    bool reflex;            /* more than half a turn */
} ring_t;

static void ring_setup(ring_t *g, int kind, float cx, float cy, float ri,
                       float ro)
{
    g->kind = kind;
    g->cx = cx;
    g->cy = cy;
    if (ri <= 0.0f) ri = -1.0f;
    g->ri = ri;
    g->ro = ro;
    g->out2 = (ro + 0.5f) * (ro + 0.5f);
    g->hole2 = ri > 0.5f ? (ri - 0.5f) * (ri - 0.5f) : -1.0f;
    g->full_out2 = ro >= 0.5f ? (ro - 0.5f) * (ro - 0.5f) : -1.0f;
    g->full_in2 = ri > -0.5f ? (ri + 0.5f) * (ri + 0.5f) : 0.0f;
    g->ro2 = ro * ro;
    g->inv2ro = 0.5f / ro;
    g->ro_approx = ro >= APPROX_R;
    g->ri2 = ri * ri;
    g->inv2ri = ri > 0.0f ? 0.5f / ri : 0.0f;
    g->ri_approx = ri >= APPROX_R;
}

/* How much of a pixel at squared distance d2 from the centre lies between
   the two radii, as the overlap of a unit interval centred at D with
   [ri, ro]: min(1/2, ro - D) + min(1/2, D - ri). The caller has already
   thrown out d2 beyond out2 or inside hole2. */
static float radial(const ring_t *g, float d2)
{
    if (d2 <= g->full_out2 && d2 >= g->full_in2) return 1.0f;

    float root = -1.0f, out, in;
    if (g->ro_approx) {
        out = (g->ro2 - d2) * g->inv2ro;
    } else {
        root = sqrtf(d2);
        out = g->ro - root;
    }
    if (g->ri <= 0.0f) {
        in = 0.5f;
    } else if (g->ri_approx) {
        in = (d2 - g->ri2) * g->inv2ri;
    } else {
        if (root < 0.0f) root = sqrtf(d2);
        in = root - g->ri;
    }
    return clamp01(minf(0.5f, out) + minf(0.5f, in));
}

/*
 * How much of a pixel lies within a butt-ended arc's angular span, from its
 * position (ax, y) in the folded frame.
 *
 * s1 is the signed distance past the cap line at +half-sweep, positive
 * beyond it; s2 the same for the mirrored cap at -half-sweep. Up to half a
 * turn the span is the wedge inside both lines, and a pixel's share of it is
 * the part of a unit interval across them that lies inside each:
 * min(1/2, -s1) + min(1/2, -s2), which also gets a sliver of an arc,
 * narrower than a pixel, right. A cap line only counts near its own end,
 * where its projection c is positive: through the centre it carries on as
 * the far side of the other cap, and would otherwise cut a false edge there.
 * Past half a turn the gap is the small wedge, so the same sum over it gives
 * what is missing.
 */
static float angular(const ring_t *g, float ax, float y)
{
    float s1 = ax * g->ch - y * g->sh;
    float c1 = ax * g->sh + y * g->ch;
    float s2 = -ax * g->ch - y * g->sh;
    float c2 = -ax * g->sh + y * g->ch;

    if (!g->reflex) {
        if (c1 <= 0.0f) return 0.0f;
        float t2 = c2 > 0.0f ? minf(0.5f, -s2) : 0.5f;
        return clamp01(minf(0.5f, -s1) + t2);
    }
    if (c1 <= 0.0f) return 1.0f;
    float t2 = c2 > 0.0f ? minf(0.5f, s2) : 0.5f;
    return 1.0f - clamp01(minf(0.5f, s1) + t2);
}

static void ring_px(canvas_t *c, const ring_t *g, int i, int y, float dy,
                    uint16_t colour)
{
    float dx = ((float)i + 0.5f) - g->cx;
    float d2 = dx * dx + dy * dy;
    if (d2 >= g->out2 || d2 <= g->hole2) return;

    if (g->kind == RING) {
        plot(c, i, y, radial(g, d2), colour);
        return;
    }

    /* Into the folded frame: x along the arc's middle turned a quarter
       clockwise, y along the middle itself, measured upward. */
    float xr = dx * g->cm + dy * g->sm;
    float yr = dx * g->sm - dy * g->cm;
    float ax = fabsf(xr);

    if (g->kind == ARC_BUTT) {
        float a = angular(g, ax, yr);
        if (a > 0.0f) plot(c, i, y, a * radial(g, d2), colour);
        return;
    }

    /* Round caps: within the span, the ring; beyond it, the distance to the
       nearer cap's centre, which the fold has made the one on this side. */
    if (ax * g->ch - yr * g->sh <= 0.0f) {
        plot(c, i, y, radial(g, d2), colour);
        return;
    }
    float ex = ax - g->capx, ey = yr - g->capy;
    float e2 = ex * ex + ey * ey;
    if (e2 >= g->cap_out2) return;
    plot(c, i, y, e2 <= g->cap_full2 ? 1.0f : band(sqrtf(e2), g->hw), colour);
}

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void ring_shade(canvas_t *c, const ring_t *g, int from, int to, int y,
                       float dy, uint16_t colour)
{
    for (int i = from; i < to; i++) ring_px(c, g, i, y, dy, colour);
}

static void ring_fill(canvas_t *c, int from, int to, int y, uint16_t colour)
{
    uint16_t *row = c->fb + (size_t)y * (size_t)c->w;
    for (int i = from; i < to; i++) row[i] = colour;
}

/*
 * Visits every pixel whose centre could be within reach of the annulus and
 * of the box [x0, x1] x [y0, y1] that holds the shape.
 *
 * Each row is cut into runs from its left end:
 *   [lo, la) edge, shaded    [la, lb) solid    [lb, s0) edge, shaded
 *   [s0, s1) the hole, skipped    [s1, ra) edge, shaded
 *   [ra, rb) solid    [rb, hi) edge, shaded
 * The solid runs, whose centres are half a pixel clear of both radii, are
 * written straight: a dial-sized disc is mostly solid, and there the cost is
 * one store a pixel. Only whole rings get them; an arc's span is decided
 * pixel by pixel. Every boundary is clamped to keep the order, which can only
 * shrink a solid run or the hole, never grow them.
 */
static void ring_draw(canvas_t *c, const ring_t *g, float x0, float y0,
                      float x1, float y1, uint16_t colour)
{
    /* Centres within half a pixel of the box:
       [floor(x0 - 1), floor(x1 + 1)). */
    int xlo = to_index(x0 - 1.0f, 0, c->w), xhi = to_index(x1 + 1.0f, 0, c->w);
    int ylo = to_index(y0 - 1.0f, 0, c->h), yhi = to_index(y1 + 1.0f, 0, c->h);

    for (int y = ylo; y < yhi; y++) {
        float dy = ((float)y + 0.5f) - g->cy;
        float dy2 = dy * dy;
        if (dy2 >= g->out2) continue;

        float half = sqrtf(g->out2 - dy2);
        int lo = to_index(g->cx - half - 0.5f, xlo, xhi);
        int hi = to_index(g->cx + half + 0.5f, xlo, xhi);

        /* Solid: centres with fi <= |dx| <= fo. */
        int la = lo, lb = lo, ra = hi, rb = hi;
        if (g->kind == RING && g->full_out2 > dy2) {
            float fo = sqrtf(g->full_out2 - dy2);
            float fi = g->full_in2 > dy2 ? sqrtf(g->full_in2 - dy2) : 0.0f;
            if (fi <= fo) {
                la = to_index(ceilf(g->cx - fo - 0.5f), lo, hi);
                lb = to_index(g->cx - fi + 0.5f, lo, hi);
                ra = to_index(ceilf(g->cx + fi - 0.5f), lo, hi);
                rb = to_index(g->cx + fo + 0.5f, lo, hi);
            }
        }
        lb = clampi(lb, la, hi);
        ra = clampi(ra, lb, hi);
        rb = clampi(rb, ra, hi);

        /* The hole: centres more than a pixel inside it on the left, any
           inside it on the right. The per-pixel test catches the rest. */
        int s0 = ra, s1 = ra;
        if (g->hole2 > dy2) {
            float hh = sqrtf(g->hole2 - dy2);
            s0 = clampi(to_index(g->cx - hh + 0.5f, lo, hi) + 1, lb, ra);
            s1 = clampi(to_index(g->cx + hh - 0.5f, lo, hi), s0, ra);
        }

        ring_shade(c, g, lo, la, y, dy, colour);
        ring_fill(c, la, lb, y, colour);
        ring_shade(c, g, lb, s0, y, dy, colour);
        ring_shade(c, g, s1, ra, y, dy, colour);
        ring_fill(c, ra, rb, y, colour);
        ring_shade(c, g, rb, hi, y, dy, colour);
    }
}

/* Finite and within a million pixels: a circle argument worth drawing. */
static bool usable(float v) { return isfinite(v) && fabsf(v) <= FAR; }

void vec_disc(canvas_t *c, float cx, float cy, float r, uint16_t colour)
{
    if (!canvas_ok(c) || !usable(cx) || !usable(cy) || !usable(r)
        || !(r > 0.0f))
        return;
    ring_t g;
    ring_setup(&g, RING, cx, cy, -1.0f, r);
    ring_draw(c, &g, cx - r, cy - r, cx + r, cy + r, colour);
}

void vec_ring(canvas_t *c, float cx, float cy, float r, float width,
              uint16_t colour)
{
    if (!canvas_ok(c) || !usable(cx) || !usable(cy) || !usable(r)
        || !usable(width) || r < 0.0f || !(width > 0.0f))
        return;
    float hw = 0.5f * width, ro = r + hw;
    ring_t g;
    ring_setup(&g, RING, cx, cy, r - hw, ro);
    ring_draw(c, &g, cx - ro, cy - ro, cx + ro, cy + ro, colour);
}

/* Widens [*x0, *x1] x [*y0, *y1] to take in the point at `angle` on the
   circle of radius r. */
static void take_in(float cx, float cy, float r, float angle, float *x0,
                    float *y0, float *x1, float *y1)
{
    float x = cx + r * sinf(angle), y = cy - r * cosf(angle);
    *x0 = minf(*x0, x);
    *x1 = maxf(*x1, x);
    *y0 = minf(*y0, y);
    *y1 = maxf(*y1, y);
}

static void arc(canvas_t *c, int kind, float cx, float cy, float r,
                float width, float a0, float a1, uint16_t colour)
{
    if (!canvas_ok(c) || !usable(cx) || !usable(cy) || !usable(r)
        || !usable(width) || r < 0.0f || !(width > 0.0f) || !isfinite(a0)
        || !isfinite(a1))
        return;

    float start = minf(a0, a1);
    float sweep = fabsf(a1 - a0);
    if (sweep >= VEC_TWO_PI) {              /* also an overflowed difference */
        vec_ring(c, cx, cy, r, width, colour);
        return;
    }
    if (kind == ARC_BUTT && !(sweep > 0.0f)) return;   /* no area at all */

    start = fmodf(start, VEC_TWO_PI);
    if (start < 0.0f) start += VEC_TWO_PI;

    float hw = 0.5f * width;
    ring_t g;
    ring_setup(&g, kind, cx, cy, r - hw, r + hw);
    g.r = r;
    g.hw = hw;
    float mid = start + 0.5f * sweep, half = 0.5f * sweep;
    g.cm = cosf(mid);
    g.sm = sinf(mid);
    g.ch = cosf(half);
    g.sh = sinf(half);
    g.capx = r * g.sh;
    g.capy = r * g.ch;
    g.cap_out2 = (hw + 0.5f) * (hw + 0.5f);
    g.cap_full2 = hw >= 0.5f ? (hw - 0.5f) * (hw - 0.5f) : -1.0f;
    g.reflex = half > 0.5f * VEC_PI;

    /* The box round the centre line: its two ends, and whichever of 12, 3,
       6 and 9 o'clock it passes, where a circle reaches furthest out. Then
       widened by the half-width, which holds the caps of either kind. */
    float x0 = cx + r * sinf(start), x1 = x0;
    float y0 = cy - r * cosf(start), y1 = y0;
    take_in(cx, cy, r, start + sweep, &x0, &y0, &x1, &y1);
    float quarter = 0.5f * VEC_PI;
    for (int k = (int)ceilf(start / quarter);
         (float)k * quarter <= start + sweep; k++)
        take_in(cx, cy, r, (float)k * quarter, &x0, &y0, &x1, &y1);

    ring_draw(c, &g, x0 - hw, y0 - hw, x1 + hw, y1 + hw, colour);
}

void vec_arc(canvas_t *c, float cx, float cy, float r, float width,
             float a0, float a1, uint16_t colour)
{
    arc(c, ARC_ROUND, cx, cy, r, width, a0, a1, colour);
}

void vec_arc_butt(canvas_t *c, float cx, float cy, float r, float width,
                  float a0, float a1, uint16_t colour)
{
    arc(c, ARC_BUTT, cx, cy, r, width, a0, a1, colour);
}

/* ---- clipping a segment ------------------------------------------------- */

/*
 * Keeps the part of segment a-b with coordinate `axis` at or below `bound`
 * (keep_low) or at or above it. False if nothing is left.
 *
 * The replacement end is found from the end that stays, not the one that
 * goes. The one that goes may be 1e30 away, and interpolating from there
 * would lose every digit that matters on the panel; from the one that stays,
 * the arithmetic keeps its precision.
 */
static bool clip_side(float a[2], float b[2], int axis, float bound,
                      bool keep_low)
{
    bool a_out = keep_low ? a[axis] > bound : a[axis] < bound;
    bool b_out = keep_low ? b[axis] > bound : b[axis] < bound;
    if (a_out && b_out) return false;
    if (!a_out && !b_out) return true;

    float *in = a_out ? b : a;
    float *out = a_out ? a : b;
    int other = 1 - axis;
    /* One end on each side, so the divisor is not zero and t is in [0, 1]. */
    float t = (bound - in[axis]) / (out[axis] - in[axis]);
    out[other] = in[other] + t * (out[other] - in[other]);
    out[axis] = bound;
    return true;
}

/* ---- lines -------------------------------------------------------------- */

void vec_line(canvas_t *c, float x0, float y0, float x1, float y1,
              float width, uint16_t colour)
{
    if (!canvas_ok(c) || !isfinite(x0) || !isfinite(y0) || !isfinite(x1)
        || !isfinite(y1) || !(width > 0.0f) || !(width <= 2.0f * FAR))
        return;

    float hw = 0.5f * width;
    float reach = hw + 0.5f;            /* past this, a pixel is untouched */
    float reach2 = reach * reach;
    float full2 = hw >= 0.5f ? (hw - 0.5f) * (hw - 0.5f) : -1.0f;

    /* Cut away whatever lies well outside the canvas. No pixel centre is
       within reach of it, so what is left draws exactly what the whole line
       would, and every number from here on is small. */
    float margin = reach + 1.0f;
    float a[2] = { pin(x0), pin(y0) }, b[2] = { pin(x1), pin(y1) };
    if (!clip_side(a, b, 0, -margin, false)
        || !clip_side(a, b, 0, (float)c->w + margin, true)
        || !clip_side(a, b, 1, -margin, false)
        || !clip_side(a, b, 1, (float)c->h + margin, true))
        return;

    float dx = b[0] - a[0], dy = b[1] - a[1];
    float len = sqrtf(dx * dx + dy * dy);
    float ux = 1.0f, uy = 0.0f;             /* a dot has no direction */
    if (len > 1e-6f) {
        ux = dx / len;
        uy = dy / len;
    } else {
        len = 0.0f;
    }

    int ylo = to_index(minf(a[1], b[1]) - reach - 0.5f, 0, c->h);
    int yhi = to_index(maxf(a[1], b[1]) + reach + 0.5f, 0, c->h);
    bool level = !(fabsf(dy) > 1e-6f);
    float inv_dy = level ? 0.0f : 1.0f / dy;    /* float division is a call */

    for (int y = ylo; y < yhi; y++) {
        float py = (float)y + 0.5f;

        /* Only the part of the segment within reach of this row, vertically,
           can light a pixel on it, so the row's pixels lie within reach of
           that part horizontally. */
        float xa = a[0], xb = b[0];
        if (!level) {
            float t0 = (py - reach - a[1]) * inv_dy;
            float t1 = (py + reach - a[1]) * inv_dy;
            if (t0 > t1) { float t = t0; t0 = t1; t1 = t; }
            t0 = maxf(t0, 0.0f);
            t1 = minf(t1, 1.0f);
            if (t0 > t1) continue;
            xa = a[0] + t0 * dx;
            xb = a[0] + t1 * dx;
        }
        int lo = to_index(minf(xa, xb) - reach - 0.5f, 0, c->w);
        int hi = to_index(maxf(xa, xb) + reach + 0.5f, 0, c->w);

        float ey = py - a[1];
        for (int i = lo; i < hi; i++) {
            float ex = ((float)i + 0.5f) - a[0];
            float along = ex * ux + ey * uy;
            float cov;
            if (along > 0.0f && along < len) {
                /* Alongside the segment: the distance is linear, no root. */
                float d = fabsf(ex * uy - ey * ux);
                if (d >= reach) continue;
                cov = band(d, hw);
            } else {
                /* Past an end: the round cap, a disc about that end. */
                float fx = along <= 0.0f ? ex : ex - dx;
                float fy = along <= 0.0f ? ey : ey - dy;
                float d2 = fx * fx + fy * fy;
                if (d2 >= reach2) continue;
                cov = d2 <= full2 ? 1.0f : band(sqrtf(d2), hw);
            }
            plot(c, i, y, cov, colour);
        }
    }
}

/* ---- polygons ----------------------------------------------------------- */

/* One edge, or part of one, confined to the strip being filled. */
typedef struct {
    float xt, yt;           /* the upper end */
    float xb, yb;           /* the lower end */
    float dxdy;             /* its run per unit of height */
    float dir;              /* +1 if it runs down the panel, -1 if up */
} piece_t;

/* Each edge leaves at most two pieces after clipping (see vec_polygon). */
static piece_t s_pieces[2 * VEC_POLY_MAX];
static int s_npieces;

/* A row's cells: each holds the change in coverage from the cell before, so
   a running sum gives the coverage. Two spare cells take the spill from an
   edge on the strip's right-hand boundary. Zero between rows. */
static float s_acc[STRIP + 2];

static void add_piece(float x0, float y0, float x1, float y1)
{
    float dir = 1.0f;
    if (y1 < y0) {
        float t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
        dir = -1.0f;
    }
    /* A piece with no height adds nothing, and a millionth of a pixel of it
       adds nothing anyone could see; dropping it also keeps dxdy finite. */
    if (!(y1 - y0 >= 1e-6f)) return;
    piece_t *p = &s_pieces[s_npieces++];
    p->xt = x0;
    p->yt = y0;
    p->xb = x1;
    p->yb = y1;
    p->dxdy = (x1 - x0) / (y1 - y0);
    p->dir = dir;
}

/*
 * Adds the signed area of one piece of edge, already confined to one row and
 * given in cell units from the strip's left edge (0 <= x <= STRIP), to the
 * row's cells. The area to the right of the edge within each cell goes to
 * that cell, as a change from the cell before; `d` is the piece's height,
 * signed by direction, so a closed outline sums to zero outside itself.
 * After font-rs's accumulation rasteriser.
 */
static void accumulate(float xa, float xb, float d, int *jmin, int *jmax)
{
    float x0 = minf(xa, xb), x1 = maxf(xa, xb);
    float x0floor = floorf(x0);
    int x0i = (int)x0floor;
    float x1ceil = ceilf(x1);
    int x1i = (int)x1ceil;
    if (x0i < *jmin) *jmin = x0i;

    if (x1i <= x0i + 1) {
        /* Within one cell: its share is the part to the right of the
           piece's mean x, and the rest carries into the next. */
        float xmf = 0.5f * (xa + xb) - x0floor;
        s_acc[x0i] += d - d * xmf;
        s_acc[x0i + 1] += d * xmf;
        if (x0i + 1 > *jmax) *jmax = x0i + 1;
        return;
    }

    /* Across several cells: a triangle in the first, a trapezium in each
       one it crosses, the remainder in the last. */
    float s = 1.0f / (x1 - x0);
    float x0f = x0 - x0floor;
    float a0 = 0.5f * s * (1.0f - x0f) * (1.0f - x0f);
    float x1f = x1 - x1ceil + 1.0f;
    float am = 0.5f * s * x1f * x1f;
    s_acc[x0i] += d * a0;
    if (x1i == x0i + 2) {
        s_acc[x0i + 1] += d * (1.0f - a0 - am);
    } else {
        float a1 = s * (1.5f - x0f);
        s_acc[x0i + 1] += d * (a1 - a0);
        for (int xi = x0i + 2; xi < x1i - 1; xi++) s_acc[xi] += d * s;
        float a2 = a1 + (float)(x1i - x0i - 3) * s;
        s_acc[x1i - 1] += d * (1.0f - a2 - am);
    }
    s_acc[x1i] += d * am;
    if (x1i > *jmax) *jmax = x1i;
}

/*
 * Clips the outline to the strip [L, R] x [T, B] into s_pieces.
 *
 * Above and below, clipping is plain: a piece of edge outside those rows
 * adds nothing to them. On the right it is plain too, since an edge's area
 * only ever reaches the cells to its right. On the left the part outside is
 * kept but flattened onto x = L: a pixel in the strip sees everything left of
 * it only as "left of me", so moving it to the boundary changes nothing
 * visible, and it keeps its height, which is what carries the winding in.
 */
static void build_pieces(const float *xy, int n, float L, float R, float T,
                         float B)
{
    s_npieces = 0;
    for (int k = 0; k < n; k++) {
        int k2 = k + 1 < n ? k + 1 : 0;
        float p[2] = { pin(xy[2 * k]), pin(xy[2 * k + 1]) };
        float q[2] = { pin(xy[2 * k2]), pin(xy[2 * k2 + 1]) };
        if (p[1] == q[1]) continue;         /* level edges carry no area */

        if (!clip_side(p, q, 1, T, false) || !clip_side(p, q, 1, B, true)
            || !clip_side(p, q, 0, R, true))
            continue;

        bool p_out = p[0] < L, q_out = q[0] < L;
        if (p_out && q_out) {
            add_piece(L, p[1], L, q[1]);
        } else if (p_out || q_out) {
            const float *in = p_out ? q : p;
            const float *out = p_out ? p : q;
            float t = (L - in[0]) / (out[0] - in[0]);
            float ym = in[1] + t * (out[1] - in[1]);
            if (p_out) {
                add_piece(L, p[1], L, ym);
                add_piece(L, ym, q[0], q[1]);
            } else {
                add_piece(p[0], p[1], L, ym);
                add_piece(L, ym, L, q[1]);
            }
        } else {
            add_piece(p[0], p[1], q[0], q[1]);
        }
    }
}

void vec_polygon(canvas_t *c, const float *xy, int n, uint16_t colour)
{
    if (!canvas_ok(c) || xy == NULL || n < 3 || n > VEC_POLY_MAX) return;

    float minx = HUGE_COORD, miny = HUGE_COORD;
    float maxx = -HUGE_COORD, maxy = -HUGE_COORD;
    for (int k = 0; k < n; k++) {
        float x = xy[2 * k], y = xy[2 * k + 1];
        if (!isfinite(x) || !isfinite(y)) return;
        x = pin(x);
        y = pin(y);
        minx = minf(minx, x);
        maxx = maxf(maxx, x);
        miny = minf(miny, y);
        maxy = maxf(maxy, y);
    }

    /* Every pixel the outline's box touches, on the canvas. */
    int col0 = to_index(minx, 0, c->w), col1 = to_index(maxx + 1.0f, 0, c->w);
    int row0 = to_index(miny, 0, c->h), row1 = to_index(maxy + 1.0f, 0, c->h);
    if (col0 >= col1 || row0 >= row1) return;

    for (int L = col0; L < col1; L += STRIP) {
        int R = L + STRIP < col1 ? L + STRIP : col1;
        int wstrip = R - L;
        float Lf = (float)L, Wf = (float)wstrip;
        build_pieces(xy, n, Lf, (float)R, (float)row0, (float)row1);
        if (s_npieces == 0) continue;

        for (int y = row0; y < row1; y++) {
            float top = (float)y, bot = top + 1.0f;
            int jmin = wstrip + 2, jmax = -1;

            for (int k = 0; k < s_npieces; k++) {
                const piece_t *p = &s_pieces[k];
                if (p->yb <= top || p->yt >= bot) continue;
                float ya = maxf(p->yt, top), yb = minf(p->yb, bot);
                if (!(yb > ya)) continue;

                /* Where the piece crosses the top and bottom of this row, kept
                   within its own ends and the strip against rounding. */
                float lox = minf(p->xt, p->xb), hix = maxf(p->xt, p->xb);
                float xa = p->xt + (ya - p->yt) * p->dxdy;
                float xb = p->xt + (yb - p->yt) * p->dxdy;
                xa = minf(maxf(xa, lox), hix) - Lf;
                xb = minf(maxf(xb, lox), hix) - Lf;
                xa = minf(maxf(xa, 0.0f), Wf);
                xb = minf(maxf(xb, 0.0f), Wf);
                accumulate(xa, xb, (yb - ya) * p->dir, &jmin, &jmax);
            }
            if (jmax < 0) continue;

            /* The running sum is the coverage, its magnitude taken so either
               winding fills, and capped at one so a region wound twice is
               filled once. Past the last changed cell the sum holds steady,
               so the row stops there if it has come back to zero. */
            float sum = 0.0f;
            for (int j = jmin; j < wstrip; j++) {
                if (j <= jmax) sum += s_acc[j];
                else if (fabsf(sum) < 1.0f / 512.0f) break;
                plot(c, L + j, y, fabsf(sum), colour);
            }
            for (int j = jmin; j <= jmax; j++) s_acc[j] = 0.0f;
        }
    }
}
