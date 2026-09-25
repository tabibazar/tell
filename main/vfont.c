#include "vfont.h"

#include "vector.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

/*
 * The glyphs.
 *
 * Every glyph is a little path on a grid where the capitals run from y = 0
 * at the top to y = 10 on the baseline, x from 0 at the left edge of the
 * ink. `w` is how far the ink reaches to the right, which is what spacing is
 * worked from, so a round letter and a straight one sit the same distance
 * from their neighbours.
 *
 * The path language is three commands, numbers separated by spaces:
 *   M x y                 start a new run at (x, y)
 *   L x y                 a straight run to (x, y)
 *   A cx cy rx ry a0 a1   an elliptical arc about (cx, cy), from angle a0 to
 *                         a1 in degrees: 0 is to the right and 90 straight
 *                         down, as the panel's y runs. a1 may be less than
 *                         a0 (anticlockwise) or more than 360 away.
 * An arc that does not start where the pen is joins it with a straight run;
 * one that starts a glyph, or follows an M at its own start, does not.
 *
 * The letterforms are a plain, slightly narrow engraver's sans: straight
 * strokes, true ellipses for the bowls, no serifs. At five pixels tall that
 * is all a dial can carry, and at twelve on the moon page it reads as
 * engraved lettering rather than as a screen font.
 *
 * Two letters are cut for the sub-dials' smallest labels rather than for
 * the page. The M is wider than its neighbours, with its vee dropped almost
 * to the baseline, and the U's bowl is deeper than a half ellipse. At five
 * pixels an M with a shallow vee is the same three strokes as an H, so MON
 * read as HON; and a U whose bowl starts high loses its foot, so SUN read as
 * SLIN.
 */
typedef struct {
    char ch;
    float w;
    const char *path;
} glyph_t;

static const glyph_t s_glyphs[] = {
    { 'A', 6.4f, "M0 10 L3.2 0 L6.4 10 M1.15 6.6 L5.25 6.6" },
    { 'B', 6.0f, "M0 5 L3.3 5 A3.3 2.5 2.4 2.5 90 -90 L0 0 L0 10 L3.4 10 "
                 "A3.4 7.5 2.6 2.5 90 -90 L0 5" },
    { 'C', 6.3f, "A3.8 5 3.8 5 -48 -312" },
    { 'D', 6.4f, "M0 0 L0 10 L2.4 10 A2.4 5 4 5 90 -90 L0 0" },
    { 'E', 5.2f, "M5.2 0 L0 0 L0 10 L5.2 10 M0 5 L4.4 5" },
    { 'F', 5.2f, "M5.2 0 L0 0 L0 10 M0 5 L4.4 5" },
    { 'G', 7.6f, "A3.8 5 3.8 5 -48 -360 L4.4 5" },
    { 'H', 6.0f, "M0 0 L0 10 M6 0 L6 10 M0 5 L6 5" },
    { 'I', 0.0f, "M0 0 L0 10" },
    { 'J', 4.4f, "M4.4 0 L4.4 7.6 A2.2 7.6 2.2 2.4 0 180" },
    { 'K', 5.8f, "M0 0 L0 10 M5.6 0 L0 6.2 M2 3.9 L5.8 10" },
    { 'L', 5.0f, "M0 0 L0 10 L5 10" },
    { 'M', 8.6f, "M0 10 L0 0 L4.3 8.8 L8.6 0 L8.6 10" },
    { 'N', 6.2f, "M0 10 L0 0 L6.2 10 L6.2 0" },
    { 'O', 7.6f, "A3.8 5 3.8 5 0 360" },
    { 'P', 5.9f, "M0 10 L0 0 L3.2 0 A3.2 2.7 2.7 2.7 -90 90 L0 5.4" },
    { 'Q', 7.6f, "A3.8 5 3.8 5 0 360 M4.6 7.2 L7.4 10.4" },
    { 'R', 6.0f, "M0 10 L0 0 L3.2 0 A3.2 2.7 2.7 2.7 -90 90 L0 5.4 "
                 "M3.1 5.4 L6 10" },
    { 'S', 6.0f, "A3 2.6 2.7 2.6 -28 -270 A3 7.45 3 2.55 -90 152" },
    { 'T', 6.2f, "M0 0 L6.2 0 M3.1 0 L3.1 10" },
    { 'U', 6.0f, "M0 0 L0 6.2 A3 6.2 3 3.8 180 0 L6 0" },
    { 'V', 6.4f, "M0 0 L3.2 10 L6.4 0" },
    { 'W', 8.8f, "M0 0 L2.2 10 L4.4 0 L6.6 10 L8.8 0" },
    { 'X', 6.0f, "M0 0 L6 10 M6 0 L0 10" },
    { 'Y', 6.2f, "M0 0 L3.1 5.2 L6.2 0 M3.1 5.2 L3.1 10" },
    { 'Z', 6.0f, "M0 0 L6 0 L0 10 L6 10" },

    { '0', 5.8f, "A2.9 5 2.9 5 0 360" },
    { '1', 2.6f, "M0 2.1 L2.6 0 L2.6 10" },
    { '2', 5.8f, "A2.9 2.9 2.8 2.9 198 398 L0 10 L5.8 10" },
    { '3', 5.8f, "A2.9 2.55 2.6 2.55 205 450 A2.9 7.4 2.9 2.6 270 522" },
    { '4', 6.0f, "M4.4 10 L4.4 0 L0 7 L6 7" },
    { '5', 5.8f, "M5.4 0 L0.9 0 L0.6 4.7 A2.9 6.9 2.9 3.1 224 510" },
    { '6', 5.8f, "M4.6 0 L0.35 5.6 M5.8 7.05 A2.9 7.05 2.9 2.95 0 360" },
    { '7', 5.8f, "M0 0 L5.8 0 L1.9 10" },
    { '8', 6.0f, "A3 2.55 2.55 2.55 0 360 M6 7.45 A3 7.45 3 2.55 0 360" },
    { '9', 5.8f, "M5.8 2.95 A2.9 2.95 2.9 2.95 0 360 M5.45 4.4 L1.2 10" },

    { ' ', 2.0f, "" },
    { '.', 0.6f, "M0.3 9.7 L0.3 9.7" },
    { ',', 1.0f, "M0.9 9.4 L0 11.6" },
    { ':', 0.6f, "M0.3 3.2 L0.3 3.2 M0.3 9.7 L0.3 9.7" },
    { '-', 3.8f, "M0 5.6 L3.8 5.6" },
    { '/', 4.4f, "M0 10.6 L4.4 -0.6" },
    { '+', 5.6f, "M0 5 L5.6 5 M2.8 2.2 L2.8 7.8" },
    { '%', 8.0f, "M3.2 2.3 A1.6 2.3 1.6 2.3 0 360 M8 7.7 A6.4 7.7 1.6 2.3 0 360 "
                 "M0.6 10 L7.4 0" },
    { '\'', 0.6f, "M0.3 0 L0.3 3" },
};

#define GLYPH_COUNT ((int)(sizeof s_glyphs / sizeof s_glyphs[0]))

/* The space between one letter's ink and the next, in grid units, before
   any tracking. */
#define GAP 2.2f

/* The most straight pieces one glyph turns into; the two full ellipses of an
   8 are the most, at 60. */
#define MAX_SEGS 96

#define DEG (3.14159265358979f / 180.0f)

static const glyph_t *find(char ch)
{
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    for (int i = 0; i < GLYPH_COUNT; i++)
        if (s_glyphs[i].ch == ch) return &s_glyphs[i];
    return NULL;
}

/* A glyph's advance in grid units: its ink plus the gap. Unknown characters
   advance as a space. */
static float advance(const glyph_t *g)
{
    return (g != NULL ? g->w : 2.0f) + GAP;
}

float vfont_width(const vfont_style_t *st, const char *s)
{
    if (st == NULL || s == NULL || s[0] == '\0') return 0.0f;
    float u = st->size / 10.0f;
    float w = 0.0f;
    for (const char *p = s; *p != '\0'; p++)
        w += advance(find(*p)) * u + st->tracking;
    /* No gap or tracking after the last letter. */
    return w - GAP * u - st->tracking;
}

/* ---- turning a path into straight pieces ------------------------------- */

typedef struct {
    float x0, y0, dx, dy, inv;   /* start, direction, 1 / |direction|^2 */
} seg_t;

typedef struct {
    seg_t seg[MAX_SEGS];
    int n;
    float px, py;                /* the pen, in panel pixels */
    bool pen;                    /* whether there is a pen position */
    /* grid unit -> panel: p = o + gx * (ax, ay) + gy * (bx, by) */
    float ox, oy, ax, ay, bx, by;
    float minx, miny, maxx, maxy;
} path_t;

static void to_panel(const path_t *p, float gx, float gy, float *x, float *y)
{
    *x = p->ox + gx * p->ax + gy * p->bx;
    *y = p->oy + gx * p->ay + gy * p->by;
}

static void add_seg(path_t *p, float x1, float y1)
{
    if (p->n >= MAX_SEGS) return;
    seg_t *s = &p->seg[p->n++];
    s->x0 = p->px;
    s->y0 = p->py;
    s->dx = x1 - p->px;
    s->dy = y1 - p->py;
    float l2 = s->dx * s->dx + s->dy * s->dy;
    s->inv = l2 > 1e-12f ? 1.0f / l2 : 0.0f;
    if (x1 < p->minx) p->minx = x1;
    if (x1 > p->maxx) p->maxx = x1;
    if (y1 < p->miny) p->miny = y1;
    if (y1 > p->maxy) p->maxy = y1;
    if (p->px < p->minx) p->minx = p->px;
    if (p->px > p->maxx) p->maxx = p->px;
    if (p->py < p->miny) p->miny = p->py;
    if (p->py > p->maxy) p->maxy = p->py;
    p->px = x1;
    p->py = y1;
}

static void line_to(path_t *p, float gx, float gy)
{
    float x, y;
    to_panel(p, gx, gy, &x, &y);
    if (!p->pen) {
        p->px = x;
        p->py = y;
        p->pen = true;
    }
    add_seg(p, x, y);
}

static void move_to(path_t *p, float gx, float gy)
{
    to_panel(p, gx, gy, &p->px, &p->py);
    p->pen = true;
}

static void arc_to(path_t *p, const float *a)
{
    float cx = a[0], cy = a[1], rx = a[2], ry = a[3];
    float a0 = a[4] * DEG, a1 = a[5] * DEG;
    /* Twelve-degree steps: on the moon page's biggest letters a chord then
       strays under a tenth of a pixel from the true curve. */
    int n = (int)ceilf(fabsf(a1 - a0) / (12.0f * DEG));
    if (n < 2) n = 2;

    float sx = cx + rx * cosf(a0), sy = cy + ry * sinf(a0);
    float x, y;
    to_panel(p, sx, sy, &x, &y);
    if (!p->pen || fabsf(x - p->px) + fabsf(y - p->py) < 1e-3f) {
        p->px = x;
        p->py = y;
        p->pen = true;
    } else {
        add_seg(p, x, y);
    }
    for (int k = 1; k <= n; k++) {
        float t = a0 + (a1 - a0) * (float)k / (float)n;
        line_to(p, cx + rx * cosf(t), cy + ry * sinf(t));
    }
}

static void build(path_t *p, const char *path)
{
    const char *s = path;
    while (*s != '\0') {
        char op = *s;
        if (op == ' ') { s++; continue; }
        s++;
        int want = op == 'A' ? 6 : 2;
        float v[6];
        for (int i = 0; i < want; i++) {
            char *end;
            v[i] = strtof(s, &end);
            if (end == s) return;           /* malformed table entry */
            s = end;
        }
        if (op == 'M') move_to(p, v[0], v[1]);
        else if (op == 'L') line_to(p, v[0], v[1]);
        else if (op == 'A') arc_to(p, v);
        else return;
    }
}

/* ---- shading ---------------------------------------------------------- */

static inline float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* Squared distance from (x, y) to the nearest centre line. */
static float nearest2(const path_t *p, float x, float y)
{
    float best = 1e30f;
    for (int i = 0; i < p->n; i++) {
        const seg_t *s = &p->seg[i];
        float ex = x - s->x0, ey = y - s->y0;
        float t = (ex * s->dx + ey * s->dy) * s->inv;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        float fx = ex - t * s->dx, fy = ey - t * s->dy;
        float d2 = fx * fx + fy * fy;
        if (d2 < best) best = d2;
    }
    return best;
}

static void shade(canvas_t *c, const path_t *p, float hw, uint16_t colour)
{
    if (p->n == 0) return;
    float reach = hw + 1.0f;
    float lim2 = (hw + 0.5f) * (hw + 0.5f);
    float x0 = floorf(p->minx - reach), x1 = ceilf(p->maxx + reach);
    float y0 = floorf(p->miny - reach), y1 = ceilf(p->maxy + reach);
    if (x1 <= 0.0f || y1 <= 0.0f || x0 >= (float)c->w || y0 >= (float)c->h)
        return;
    int ix0 = x0 < 0.0f ? 0 : (int)x0;
    int iy0 = y0 < 0.0f ? 0 : (int)y0;
    int ix1 = x1 > (float)c->w ? c->w : (int)x1;
    int iy1 = y1 > (float)c->h ? c->h : (int)y1;

    for (int j = iy0; j < iy1; j++) {
        uint16_t *row = c->fb + (size_t)j * (size_t)c->w;
        float y = (float)j + 0.5f;
        for (int i = ix0; i < ix1; i++) {
            float x = (float)i + 0.5f;
            float d2 = nearest2(p, x, y);
            if (d2 >= lim2) continue;
            /* The overlap of a unit interval centred d from the centre
               line with the stroke's [-hw, hw], as vector.c shades bands:
               exact square-on, and a thin stroke keeps its true weight. */
            float d = sqrtf(d2);
            float h1 = hw - d < 0.5f ? hw - d : 0.5f;
            float h2 = hw + d < 0.5f ? hw + d : 0.5f;
            float cov = clamp01(h1 + h2);
            int a = (int)(cov * 255.0f + 0.5f);
            if (a <= 0) continue;
            row[i] = a >= 255 ? colour : vec_blend(row[i], colour, (uint8_t)a);
        }
    }
}

void vfont_draw(canvas_t *c, const vfont_style_t *st, float x, float y,
                float angle, int align, uint16_t colour, const char *s)
{
    if (c == NULL || c->fb == NULL || c->w <= 0 || c->h <= 0 || st == NULL
        || s == NULL)
        return;
    if (!isfinite(x) || !isfinite(y) || !isfinite(angle)
        || !isfinite(st->size) || !isfinite(st->weight)
        || !isfinite(st->tracking) || !(st->size > 0.0f)
        || !(st->weight > 0.0f))
        return;
    /* Far off the panel nothing can land, and float cannot place a stroke
       out there anyway. */
    if (fabsf(x) > 1e5f || fabsf(y) > 1e5f || st->size > 1e4f
        || st->weight > 1e3f || fabsf(st->tracking) > 1e4f)
        return;

    float u = st->size / 10.0f;
    float ca = cosf(angle), sa = sinf(angle);
    float width = vfont_width(st, s);
    float pen = align < 0 ? 0.0f : (align > 0 ? -width : -0.5f * width);
    float hw = 0.5f * st->weight;

    for (const char *q = s; *q != '\0'; q++) {
        const glyph_t *g = find(*q);
        if (g != NULL && g->path[0] != '\0') {
            /* Static, like vector.c's polygon rows: two kilobytes is too
               much to ask of a small task's stack. Not reentrant. */
            static path_t p;
            p.n = 0;
            p.pen = false;
            p.px = p.py = 0.0f;
            /* Text-local frame: x along the line from the anchor, y down,
               the cap middle on y = 0. Turned by `angle`, clockwise. */
            float lx = pen, ly = -0.5f * st->size;
            p.ox = x + lx * ca - ly * sa;
            p.oy = y + lx * sa + ly * ca;
            p.ax = u * ca;
            p.ay = u * sa;
            p.bx = -u * sa;
            p.by = u * ca;
            p.minx = p.miny = 1e30f;
            p.maxx = p.maxy = -1e30f;
            build(&p, g->path);
            shade(c, &p, hw, colour);
        }
        pen += advance(g) * u + st->tracking;
    }
}
