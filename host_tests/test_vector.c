#include "vector.h"

#include <float.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * The vector drawing, on watch's 240x280 panel. The framebuffer sits between
 * two guard bands of a known pattern, and every primitive, however far off the
 * canvas it is thrown, must leave them alone.
 *
 * Coverage is measured by drawing white on black and reading the green
 * channel back, 0..63: summed over the canvas it is the area drawn, which
 * must come out at the shape's true area.
 */

#define W 240
#define H 280
#define GUARD 256
#define CANARY 0xA5C3

#define PI_F 3.14159265f

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

static uint16_t buf[GUARD + W * H + GUARD];
static canvas_t cv;

static void canvas_setup(canvas_t *c, uint16_t *fb, int w, int h)
{
    memset(c, 0, sizeof *c);
    c->fb = fb;
    c->w = w;
    c->h = h;
    c->scale = 1;
}

/* Clears the panel to `bg` and repaints the guards. */
static void reset(uint16_t bg)
{
    for (int i = 0; i < GUARD; i++) {
        buf[i] = CANARY;
        buf[GUARD + W * H + i] = CANARY;
    }
    for (int i = 0; i < W * H; i++) buf[GUARD + i] = bg;
    canvas_setup(&cv, buf + GUARD, W, H);
}

static int guards_intact(void)
{
    for (int i = 0; i < GUARD; i++)
        if (buf[i] != CANARY || buf[GUARD + W * H + i] != CANARY) return 0;
    return 1;
}

static uint16_t px(int x, int y) { return cv.fb[y * W + x]; }

static int green(uint16_t v) { return (v >> 5) & 0x3F; }

/* Area drawn, in pixels, from white on black. */
static double area(const canvas_t *c)
{
    double sum = 0.0;
    for (int i = 0; i < c->w * c->h; i++) sum += green(c->fb[i]) / 63.0;
    return sum;
}

static int near_rel(double got, double want, double tol)
{
    double err = fabs(got - want) / want;
    if (err > tol) printf("     got %.2f want %.2f (%.2f%%)\n", got, want,
                          100.0 * err);
    return err <= tol;
}

static int count_not(uint16_t v)
{
    int n = 0;
    for (int i = 0; i < W * H; i++) if (cv.fb[i] != v) n++;
    return n;
}

/* True if every pixel that changed from `bg` lies in [x0,x1] x [y0,y1]. */
static int changes_within(uint16_t bg, int x0, int y0, int x1, int y1)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (px(x, y) != bg && (x < x0 || x > x1 || y < y0 || y > y1))
                return 0;
    return 1;
}

/* ---- blending ----------------------------------------------------------- */

static void test_blend(void)
{
    static const uint16_t cols[] = { 0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F,
                                     0x8410, 0x1234, 0xFEDC, 0xA5C3 };
    int n = (int)(sizeof cols / sizeof cols[0]);
    int ends = 1, rounded = 1, monotone = 1;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            uint16_t d = cols[i], s = cols[j];
            if (vec_blend(d, s, 0) != d) ends = 0;
            if (vec_blend(d, s, 255) != s) ends = 0;
            int prev_g = -1;
            for (int a = 0; a <= 255; a++) {
                uint16_t v = vec_blend(d, s, (uint8_t)a);
                /* Each channel within half a step of the exact mix. */
                double fr = ((d >> 11) * (255 - a) + (s >> 11) * a) / 255.0;
                double fg = (((d >> 5) & 63) * (255 - a)
                             + ((s >> 5) & 63) * a) / 255.0;
                double fb = ((d & 31) * (255 - a) + (s & 31) * a) / 255.0;
                if (fabs((v >> 11) - fr) > 0.5 || fabs(green(v) - fg) > 0.5
                    || fabs((v & 31) - fb) > 0.5)
                    rounded = 0;
                /* Green heads steadily from dst's towards src's. */
                int g = green(v);
                int up = green(s) >= green(d);
                if (prev_g >= 0 && (up ? g < prev_g : g > prev_g))
                    monotone = 0;
                prev_g = g;
            }
        }
    }
    expect("blend: alpha 0 keeps dst, 255 gives src, exactly", ends);
    expect("blend: every channel rounded to the nearest step", rounded);
    expect("blend: moves steadily from dst to src", monotone);
    expect("blend: half of white on black is mid grey",
           vec_blend(0x0000, 0xFFFF, 128) == ((16 << 11) | (32 << 5) | 16));
}

/* ---- blending in linear light -------------------------------------------- */

static double light(unsigned code, unsigned max) { return pow((double)code / max, 2.2); }

/* The two lights mixed, the result encoded and rounded in the encoded scale. */
static unsigned ref_linear(unsigned d, unsigned s, unsigned a, unsigned max)
{
    double l = light(d, max) * (255 - a) / 255.0 + light(s, max) * a / 255.0;
    return (unsigned)floor(pow(l, 1 / 2.2) * max + 0.5);
}

static void test_blend_linear(void)
{
    /* Every channel pair at every alpha, red (5 bits) and green (6): within a
       code of the arithmetic, the odd tie in the tables' rounding aside. */
    long exact = 0, off1 = 0, worse = 0;
    for (unsigned a = 0; a <= 255; a++)
        for (unsigned d = 0; d < 64; d++)
            for (unsigned s = 0; s < 64; s++) {
                unsigned g = green(vec_blend_linear((uint16_t)(d << 5), (uint16_t)(s << 5), (uint8_t)a));
                int diff = (int)g - (int)ref_linear(d, s, a, 63);
                if (diff == 0) exact++; else if (diff == 1 || diff == -1) off1++; else worse++;
                if (d < 32 && s < 32) {
                    unsigned r = vec_blend_linear((uint16_t)(d << 11), (uint16_t)(s << 11), (uint8_t)a) >> 11;
                    diff = (int)r - (int)ref_linear(d, s, a, 31);
                    if (diff == 0) exact++; else if (diff == 1 || diff == -1) off1++; else worse++;
                }
            }
    printf("     linear blend: %ld exact, %ld one code off, %ld worse\n", exact, off1, worse);
    expect("linear blend: linear-light arithmetic, never more than a code out", worse == 0);
    expect("linear blend: exact but for the odd tie", off1 * 1000 < exact);
    expect("linear blend: alpha 0 keeps dst, 255 gives src",
           vec_blend_linear(0x1234, 0xFEDC, 0) == 0x1234 && vec_blend_linear(0x1234, 0xFEDC, 255) == 0xFEDC);
    /* Half-covered white on black: half the light, 0.5^(1/2.2) = 73 % of
       full code, where mixing the codes gives 50 %. */
    expect("linear blend: half of white on black is half the light",
           (vec_blend_linear(0x0000, 0xFFFF, 128) >> 11) == 23);

    /* Off by default, so the watch face is drawn exactly as it always was;
       on, a shape's edges are the linear blend's; and the switch hands back
       what it replaced. */
    expect("linear light is off unless asked for", vec_linear_light(false) == false);
    reset(0);
    vec_disc(&cv, 60.0f, 60.0f, 20.3f, 0xFFFF);
    uint16_t gamma_edge = px(80, 60);
    int changed = 0, brighter = 1;
    static uint16_t before[W * H];
    memcpy(before, cv.fb, sizeof before);
    bool was = vec_linear_light(true);
    reset(0);
    vec_disc(&cv, 60.0f, 60.0f, 20.3f, 0xFFFF);
    for (int i = 0; i < W * H; i++) {
        if (cv.fb[i] != before[i]) changed++;
        if (green(cv.fb[i]) < green(before[i])) brighter = 0;
    }
    uint16_t linear_edge = px(80, 60);
    expect("the switch hands back the setting it replaced", was == false && vec_linear_light(false) == true);
    printf("     a disc's edge pixel: %04X mixing codes, %04X in linear light\n", gamma_edge, linear_edge);
    expect("in linear light a white edge over black is brighter, nowhere darker",
           changed > 0 && brighter && green(linear_edge) > green(gamma_edge));
    reset(0);
    vec_disc(&cv, 60.0f, 60.0f, 20.3f, 0xFFFF);
    expect("and off again, the same pixels as before", memcmp(before, cv.fb, sizeof before) == 0);
}

/* ---- the pixel convention ---------------------------------------------- */

static void test_convention(void)
{
    /* A one-pixel line along y = 20.5 fills row 20 and nothing else. */
    reset(0);
    vec_line(&cv, 10.0f, 20.5f, 50.0f, 20.5f, 1.0f, 0xFFFF);
    expect("line on y=20.5 fills row 20", px(30, 20) == 0xFFFF);
    expect("... and leaves rows 19 and 21",
           px(30, 19) == 0 && px(30, 21) == 0);

    /* The same line on a pixel boundary lights both rows by half. */
    reset(0);
    vec_line(&cv, 10.0f, 21.0f, 50.0f, 21.0f, 1.0f, 0xFFFF);
    expect("line on y=21 lights rows 20 and 21 half each",
           abs(green(px(30, 20)) - 32) <= 1 && abs(green(px(30, 21)) - 32) <= 1);
    expect("... and not rows 19 or 22", px(30, 19) == 0 && px(30, 22) == 0);

    /* A disc on the panel's centre (120, 140) is symmetric about both axes,
       since that point is a pixel corner. */
    reset(0);
    vec_disc(&cv, 120.0f, 140.0f, 30.3f, 0xFFFF);
    int sym = 1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (px(x, y) != px(W - 1 - x, y) || px(x, y) != px(x, H - 1 - y))
                sym = 0;
    expect("disc at (120, 140) is mirror-symmetric on 240x280", sym);
}

/* ---- circles ------------------------------------------------------------ */

static void test_disc(void)
{
    static const float cases[][3] = {
        { 120.0f, 140.0f, 40.0f }, { 77.3f, 101.9f, 23.7f },
        { 150.25f, 60.6f, 9.4f }, { 60.5f, 200.5f, 5.0f },
        { 180.1f, 220.7f, 2.5f }, { 120.0f, 140.0f, 100.0f },
    };
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        float cx = cases[k][0], cy = cases[k][1], r = cases[k][2];
        reset(0);
        vec_disc(&cv, cx, cy, r, 0xFFFF);
        char what[96];
        snprintf(what, sizeof what, "disc r=%.1f covers pi r^2", r);
        expect(what, near_rel(area(&cv), PI_F * r * r, r >= 5.0f ? 0.01 : 0.03));
        snprintf(what, sizeof what, "disc r=%.1f writes only within r+1", r);
        expect(what, changes_within(0, (int)floorf(cx - r - 1), (int)floorf(cy - r - 1),
                                    (int)floorf(cx + r + 1), (int)floorf(cy + r + 1)));
    }

    /* Over another colour: solid inside, untouched outside. */
    reset(0x1234);
    vec_disc(&cv, 100.3f, 120.8f, 20.0f, 0xFEDC);
    expect("disc: its middle is exactly the colour", px(100, 120) == 0xFEDC);
    expect("disc: a pixel just past r + 1 is untouched", px(100, 142) == 0x1234);
    expect("disc: guards intact", guards_intact());

    reset(0);
    vec_disc(&cv, 120.0f, 140.0f, 0.0f, 0xFFFF);
    vec_disc(&cv, 120.0f, 140.0f, -5.0f, 0xFFFF);
    expect("disc: radius zero or negative draws nothing", count_not(0) == 0);
}

static void test_ring(void)
{
    reset(0);
    vec_ring(&cv, 120.4f, 139.7f, 60.0f, 3.0f, 0xFFFF);
    expect("ring r=60 w=3 covers 2 pi r w", near_rel(area(&cv), 2 * PI_F * 60 * 3, 0.01));
    expect("ring: the hole is untouched", px(120, 140) == 0);
    expect("ring: on the circle is solid", px(120, 80) == 0xFFFF);

    reset(0);
    vec_ring(&cv, 120.4f, 139.7f, 50.0f, 0.5f, 0xFFFF);
    expect("ring: a half-pixel ring keeps its weight",
           near_rel(area(&cv), 2 * PI_F * 50 * 0.5, 0.04));

    reset(0);
    vec_ring(&cv, 100.0f, 100.0f, 3.0f, 10.0f, 0xFFFF);
    expect("ring: wider than its radius is a disc of r + w/2",
           near_rel(area(&cv), PI_F * 8 * 8, 0.01));
}

/* ---- arcs --------------------------------------------------------------- */

static uint16_t at_angle(float cx, float cy, float r, float a)
{
    int x = (int)floorf(cx + r * sinf(a)), y = (int)floorf(cy - r * cosf(a));
    return px(x, y);
}

static void test_arc(void)
{
    const float cx = 120.0f, cy = 140.0f, r = 70.0f, w = 6.0f;

    /* 12 to 3 o'clock, the way a hand turns. */
    reset(0);
    vec_arc(&cv, cx, cy, r, w, 0.0f, PI_F / 2, 0xFFFF);
    expect("arc 0..pi/2 lights half past one", at_angle(cx, cy, r, PI_F / 4) == 0xFFFF);
    expect("arc 0..pi/2 leaves half past seven", at_angle(cx, cy, r, 5 * PI_F / 4) == 0);
    expect("arc 0..pi/2 leaves half past ten", at_angle(cx, cy, r, -PI_F / 4) == 0);
    expect("arc 0..pi/2 leaves half past four", at_angle(cx, cy, r, 3 * PI_F / 4) == 0);
    double round_area = area(&cv);
    expect("round arc covers sweep r w + the two half-disc caps",
           near_rel(round_area, (PI_F / 2) * r * w + PI_F * 3 * 3, 0.01));

    /* The order of the angles does not matter. */
    static uint16_t first[W * H];
    memcpy(first, cv.fb, sizeof first);
    reset(0);
    vec_arc(&cv, cx, cy, r, w, PI_F / 2, 0.0f, 0xFFFF);
    expect("arc: a1 < a0 draws the same arc", memcmp(first, cv.fb, sizeof first) == 0);

    /* Whole turns are dropped. */
    reset(0);
    vec_arc(&cv, cx, cy, r, w, 4 * PI_F, 4 * PI_F + PI_F / 2, 0xFFFF);
    int diff = 0;
    for (int i = 0; i < W * H; i++)
        if (abs(green(cv.fb[i]) - green(first[i])) > 1) diff++;
    expect("arc: 4 pi .. 4 pi + pi/2 is the same arc", diff == 0);

    /* Negative angles run anticlockwise: -pi/2 is 9 o'clock. */
    reset(0);
    vec_arc(&cv, cx, cy, r, w, -PI_F / 2, 0.0f, 0xFFFF);
    expect("arc -pi/2..0 lights half past ten", at_angle(cx, cy, r, -PI_F / 4) == 0xFFFF);
    expect("arc -pi/2..0 leaves half past one", at_angle(cx, cy, r, PI_F / 4) == 0);

    /* Butt caps: the annular sector's area exactly, for spans either side of
       half a turn and exactly on it. */
    static const float sweeps[] = { 0.3f, 1.0f, PI_F / 2, PI_F, 4.0f, 5.9f };
    for (size_t k = 0; k < sizeof sweeps / sizeof sweeps[0]; k++) {
        reset(0);
        vec_arc_butt(&cv, cx + 0.3f, cy - 0.2f, r, w, 0.7f, 0.7f + sweeps[k], 0xFFFF);
        char what[96];
        snprintf(what, sizeof what, "butt arc of %.2f rad covers sweep r w", sweeps[k]);
        expect(what, near_rel(area(&cv), sweeps[k] * r * w, 0.01));
    }

    /* Two butt arcs that meet make the ring: the caps' anti-aliasing is
       complementary, so their areas add up to the ring's. */
    reset(0);
    vec_arc_butt(&cv, cx, cy, 50.0f, 4.0f, 1.1f, 3.0f, 0xFFFF);
    double part1 = area(&cv);
    reset(0);
    vec_arc_butt(&cv, cx, cy, 50.0f, 4.0f, 3.0f, 1.1f + 2 * PI_F, 0xFFFF);
    double part2 = area(&cv);
    reset(0);
    vec_ring(&cv, cx, cy, 50.0f, 4.0f, 0xFFFF);
    expect("butt arcs that meet add up to the ring", near_rel(part1 + part2, area(&cv), 0.005));

    /* A tiny butt arc keeps the weight of its length, round caps or not. */
    reset(0);
    vec_arc_butt(&cv, cx, cy, 60.0f, 2.0f, 1.0f, 1.0f + 0.4f / 60.0f, 0xFFFF);
    expect("butt arc 0.4 px long covers 0.8 px^2", fabs(area(&cv) - 0.8) < 0.25);

    reset(0);
    vec_arc_butt(&cv, cx, cy, r, w, 1.0f, 1.0f, 0xFFFF);
    expect("butt arc of zero sweep draws nothing", count_not(0) == 0);

    reset(0);
    vec_arc(&cv, cx, cy, r, w, 1.0f, 1.0f, 0xFFFF);
    expect("round arc of zero sweep is a dot of the width",
           near_rel(area(&cv), PI_F * 3 * 3, 0.05));

    /* A full turn or more is the ring itself. */
    reset(0);
    vec_ring(&cv, cx, cy, r, w, 0xFFFF);
    memcpy(first, cv.fb, sizeof first);
    reset(0);
    vec_arc(&cv, cx, cy, r, w, 0.5f, 0.5f + 2 * PI_F + 0.01f, 0xFFFF);
    expect("arc of more than 2 pi is the ring", memcmp(first, cv.fb, sizeof first) == 0);

    /* A short arc touches only its own corner of the panel. */
    reset(0);
    vec_arc(&cv, cx, cy, r, w, 0.2f, 0.9f, 0xFFFF);
    expect("arc 0.2..0.9 writes only in the upper right",
           changes_within(0, (int)cx, (int)(cy - r - w), W - 1, (int)cy));
    expect("arc: guards intact", guards_intact());
}

/* ---- lines -------------------------------------------------------------- */

static void test_line(void)
{
    static const float cases[][5] = {
        { 30.3f, 40.7f, 190.2f, 120.9f, 5.0f },
        { 20.0f, 250.0f, 200.0f, 30.0f, 3.0f },
        { 120.0f, 20.0f, 120.0f, 260.0f, 2.0f },
        { 15.5f, 99.25f, 225.75f, 99.25f, 7.5f },
        { 50.0f, 60.0f, 170.0f, 220.0f, 1.0f },
        { 40.0f, 200.0f, 200.0f, 90.0f, 0.5f },
    };
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        const float *l = cases[k];
        reset(0);
        vec_line(&cv, l[0], l[1], l[2], l[3], l[4], 0xFFFF);
        float len = hypotf(l[2] - l[0], l[3] - l[1]), hw = l[4] / 2;
        char what[96];
        snprintf(what, sizeof what, "line w=%.1f covers length*width + round caps", l[4]);
        expect(what, near_rel(area(&cv), len * l[4] + PI_F * hw * hw,
                              l[4] >= 1.0f ? 0.02 : 0.05));
        int x0 = (int)floorf(fminf(l[0], l[2]) - hw - 1);
        int x1 = (int)floorf(fmaxf(l[0], l[2]) + hw + 1);
        int y0 = (int)floorf(fminf(l[1], l[3]) - hw - 1);
        int y1 = (int)floorf(fmaxf(l[1], l[3]) + hw + 1);
        snprintf(what, sizeof what, "line w=%.1f writes only in its box", l[4]);
        expect(what, changes_within(0, x0, y0, x1, y1));
    }

    reset(0);
    vec_line(&cv, 100.0f, 100.0f, 100.0f, 100.0f, 8.0f, 0xFFFF);
    expect("line of zero length is a dot of the width", near_rel(area(&cv), PI_F * 16, 0.02));

    reset(0);
    vec_line(&cv, 10, 10, 200, 200, 0.0f, 0xFFFF);
    vec_line(&cv, 10, 10, 200, 200, -3.0f, 0xFFFF);
    expect("line of no width draws nothing", count_not(0) == 0);

    /* Thick lines are solid down the middle and the caps are round. */
    reset(0);
    vec_line(&cv, 60.0f, 140.0f, 180.0f, 140.0f, 10.0f, 0xFFFF);
    expect("thick line: solid on its axis", px(120, 140) == 0xFFFF && px(120, 136) == 0xFFFF);
    expect("thick line: round cap is solid 3.5 px past the end", px(56, 140) == 0xFFFF);
    expect("thick line: the cap's corner is cut", px(55, 135) == 0);
}

/* ---- polygons ----------------------------------------------------------- */

static double shoelace(const float *xy, int n)
{
    double a = 0.0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        a += (double)xy[2 * i] * xy[2 * j + 1] - (double)xy[2 * j] * xy[2 * i + 1];
    }
    return fabs(a) / 2.0;
}

/* Reference inside test for a simple polygon: even-odd ray crossing. */
static int inside(const float *xy, int n, float x, float y)
{
    int in = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float xi = xy[2 * i], yi = xy[2 * i + 1], xj = xy[2 * j], yj = xy[2 * j + 1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
            in = !in;
    }
    return in;
}

static float seg_dist(float px_, float py_, float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay;
    float t = ((px_ - ax) * dx + (py_ - ay) * dy) / (dx * dx + dy * dy);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return hypotf(px_ - ax - t * dx, py_ - ay - t * dy);
}

/* Every pixel whose centre is more than a pixel from every edge must be
   exactly the colour if inside and untouched if outside. */
static int agrees_with_reference(const float *xy, int n, uint16_t colour, uint16_t bg)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float cx = x + 0.5f, cy = y + 0.5f, d = 1e9f;
            for (int i = 0; i < n; i++) {
                int j = (i + 1) % n;
                float e = seg_dist(cx, cy, xy[2 * i], xy[2 * i + 1], xy[2 * j], xy[2 * j + 1]);
                if (e < d) d = e;
            }
            if (d <= 1.0f) continue;
            uint16_t want = inside(xy, n, cx, cy) ? colour : bg;
            if (px(x, y) != want) {
                printf("     pixel (%d,%d) is %04x, want %04x\n", x, y, px(x, y), want);
                return 0;
            }
        }
    }
    return 1;
}

static void reversed(const float *xy, int n, float *out)
{
    for (int i = 0; i < n; i++) {
        out[2 * i] = xy[2 * (n - 1 - i)];
        out[2 * i + 1] = xy[2 * (n - 1 - i) + 1];
    }
}

static void test_polygon(void)
{
    /* A U on whole pixels: every pixel is all or nothing, and the notch is
       empty. */
    static const float u[] = { 10, 10, 50, 10, 50, 50, 40, 50, 40, 20, 20, 20, 20, 50, 10, 50 };
    reset(0);
    vec_polygon(&cv, u, 8, 0xFFFF);
    expect("U on whole pixels: area exactly 1000", fabs(area(&cv) - 1000.0) < 0.01);
    expect("U: the notch is empty", px(30, 30) == 0 && px(21, 49) == 0 && px(38, 21) == 0);
    expect("U: the arms are solid", px(15, 45) == 0xFFFF && px(45, 45) == 0xFFFF && px(30, 15) == 0xFFFF);
    int partial = 0;
    for (int i = 0; i < W * H; i++) if (cv.fb[i] != 0 && cv.fb[i] != 0xFFFF) partial++;
    expect("U: no pixel partly covered", partial == 0);

    /* A five-pointed star as a ten-point concave outline, off the grid. */
    float star[20];
    for (int i = 0; i < 10; i++) {
        float a = i * PI_F / 5, rr = (i & 1) ? 38.2f : 100.0f;
        star[2 * i] = 120.3f + rr * sinf(a);
        star[2 * i + 1] = 140.6f - rr * cosf(a);
    }
    reset(0);
    vec_polygon(&cv, star, 10, 0xFFFF);
    expect("star: area matches the shoelace formula", near_rel(area(&cv), shoelace(star, 10), 0.005));
    expect("star: inside solid, outside untouched, away from the edges",
           agrees_with_reference(star, 10, 0xFFFF, 0));
    static uint16_t first[W * H];
    memcpy(first, cv.fb, sizeof first);

    float back[20];
    reversed(star, 10, back);
    reset(0);
    vec_polygon(&cv, back, 10, 0xFFFF);
    int diff = 0;
    for (int i = 0; i < W * H; i++) if (abs(green(cv.fb[i]) - green(first[i])) > 1) diff++;
    expect("star: wound the other way, the same fill", diff == 0);

    /* A leaf, as a hand would be: sharp tip, concave waist. */
    static const float leaf[] = { 120, 30, 131, 90, 126, 130, 128, 150, 120, 158,
                                  112, 150, 114, 130, 109, 90 };
    reset(0x2104);
    vec_polygon(&cv, leaf, 8, 0xE6A0);
    expect("leaf: inside solid, outside untouched, away from the edges",
           agrees_with_reference(leaf, 8, 0xE6A0, 0x2104));
    expect("leaf: writes only in its box", changes_within(0x2104, 108, 29, 131, 158));

    /* A self-crossing pentagram: nonzero winding fills the middle, where the
       outline goes round twice. */
    float pent[10];
    for (int i = 0; i < 5; i++) {
        float a = i * 4 * PI_F / 5;
        pent[2 * i] = 120 + 90 * sinf(a);
        pent[2 * i + 1] = 140 - 90 * cosf(a);
    }
    reset(0);
    vec_polygon(&cv, pent, 5, 0xFFFF);
    expect("pentagram: nonzero rule fills the middle", px(120, 140) == 0xFFFF);
    expect("pentagram: and the points", px(120, 60) == 0xFFFF);

    /* A square with a square hole, the hole wound the other way and joined by
       a slit whose two sides cancel. */
    static const float holed[] = { 10, 10, 110, 10, 110, 110, 10, 110, 10, 10,
                                   30, 30, 30, 90, 90, 90, 90, 30, 30, 30 };
    reset(0);
    vec_polygon(&cv, holed, 10, 0xFFFF);
    expect("holed square: area 100^2 - 60^2", fabs(area(&cv) - 6400.0) < 0.5);
    expect("holed square: the hole is empty", px(60, 60) == 0);

    /* The most points allowed, centred ten pixels in from the left edge, so
       almost every edge crosses x = 0 and splits in two: the fill's piece
       buffer runs to its limit. The area must be the part with x >= 0. */
    float gon[2 * VEC_POLY_MAX], kept[4 * VEC_POLY_MAX];
    for (int i = 0; i < VEC_POLY_MAX; i++) {
        float a = i * 2 * PI_F / VEC_POLY_MAX + 0.01f;
        gon[2 * i] = 10.0f + 40.0f * sinf(a);
        gon[2 * i + 1] = 140.0f - 40.0f * cosf(a);
    }
    int nk = 0;                     /* Sutherland-Hodgman against x >= 0 */
    for (int i = 0; i < VEC_POLY_MAX; i++) {
        int j = (i + 1) % VEC_POLY_MAX;
        float ax = gon[2 * i], ay = gon[2 * i + 1], bx = gon[2 * j], by = gon[2 * j + 1];
        if (ax >= 0) { kept[2 * nk] = ax; kept[2 * nk + 1] = ay; nk++; }
        if ((ax >= 0) != (bx >= 0)) {
            kept[2 * nk] = 0;
            kept[2 * nk + 1] = ay + (0 - ax) / (bx - ax) * (by - ay);
            nk++;
        }
    }
    reset(0);
    vec_polygon(&cv, gon, VEC_POLY_MAX, 0xFFFF);
    expect("64-gon across the left edge: area of the part on the panel",
           near_rel(area(&cv), shoelace(kept, nk), 0.002));
    expect("64-gon across the left edge: guards intact", guards_intact());

    /* Degenerate arguments. */
    reset(0);
    vec_polygon(&cv, u, 2, 0xFFFF);
    vec_polygon(&cv, NULL, 8, 0xFFFF);
    vec_polygon(&cv, u, VEC_POLY_MAX + 1, 0xFFFF);
    static const float flat[] = { 10, 10, 100, 100, 200, 200 };
    vec_polygon(&cv, flat, 3, 0xFFFF);
    expect("polygon: n < 3, NULL, too many points or no area draws nothing", count_not(0) == 0);
}

/* A polygon wider than one strip of the fill, on a wide canvas. */
static void test_polygon_strips(void)
{
    enum { WW = 800, WH = 60 };
    static uint16_t wide[WW * WH];
    canvas_t c;
    canvas_setup(&c, wide, WW, WH);
    memset(wide, 0, sizeof wide);
    static const float rect[] = { 3.25f, 10, 790.75f, 10, 790.75f, 50, 3.25f, 50 };
    vec_polygon(&c, rect, 4, 0xFFFF);
    expect("wide rectangle: area exact across strips", fabs(area(&c) - 787.5 * 40) < 0.5);
    int seams = 1;
    for (int x = 4; x < 790; x++)
        if (wide[30 * WW + x] != 0xFFFF) seams = 0;
    expect("wide rectangle: no seams between strips", seams);
    expect("wide rectangle: left edge pixel three quarters covered",
           abs(green(wide[30 * WW + 3]) - 47) <= 1);
    expect("wide rectangle: right edge pixel three quarters covered",
           abs(green(wide[30 * WW + 790]) - 47) <= 1);

    /* A slanted wide triangle, whose edges cross the strip boundaries. */
    memset(wide, 0, sizeof wide);
    static const float tri[] = { 0.5f, 55.5f, 799.5f, 4.25f, 700.0f, 58.0f };
    vec_polygon(&c, tri, 3, 0xFFFF);
    expect("wide triangle: area matches the shoelace formula",
           near_rel(area(&c), shoelace(tri, 3), 0.002));
}

/* ---- clipping ----------------------------------------------------------- */

/*
 * Each primitive drawn half off the panel must show exactly the part of it a
 * bigger canvas would show there. The big canvas is the panel with a panel's
 * width and height of margin on every side; coordinates on it are shifted by
 * whole pixels, and are multiples of 1/64, so the arithmetic is the same.
 */
enum { BW = 3 * W, BH = 3 * H };
static uint16_t bigbuf[BW * BH];

typedef void (*draw_fn)(canvas_t *c, float ox, float oy);

static void d_disc(canvas_t *c, float ox, float oy) { vec_disc(c, ox - 13.375f, oy + 50.25f, 40.5f, 0xFFFF); }
static void d_ring(canvas_t *c, float ox, float oy) { vec_ring(c, ox + 230.5f, oy + 270.25f, 35.0f, 4.0f, 0xFFFF); }
static void d_arc(canvas_t *c, float ox, float oy) { vec_arc(c, ox + 120.0f, oy - 20.0f, 60.0f, 7.0f, 1.0f, 5.0f, 0xFFFF); }
static void d_butt(canvas_t *c, float ox, float oy) { vec_arc_butt(c, ox + 250.0f, oy + 140.0f, 45.0f, 9.0f, 3.5f, 6.0f, 0xFFFF); }
static void d_line(canvas_t *c, float ox, float oy) { vec_line(c, ox - 60.25f, oy + 30.5f, ox + 300.75f, oy + 240.125f, 6.0f, 0xFFFF); }
static void d_line2(canvas_t *c, float ox, float oy) { vec_line(c, ox + 100.0f, oy - 40.0f, ox + 130.5f, oy + 10.25f, 3.5f, 0xFFFF); }
static void d_poly(canvas_t *c, float ox, float oy)
{
    float p[] = { ox - 80, oy + 20, ox + 150.5f, oy - 30.25f, ox + 100, oy + 90,
                  ox + 320.75f, oy + 200, ox + 60, oy + 350, ox + 40, oy + 120 };
    vec_polygon(c, p, 6, 0xFFFF);
}

static void test_clip_exact(void)
{
    static const struct { const char *name; draw_fn fn; } cases[] = {
        { "disc", d_disc }, { "ring", d_ring }, { "round arc", d_arc },
        { "butt arc", d_butt }, { "line", d_line }, { "short line", d_line2 },
        { "polygon", d_poly },
    };
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        reset(0);
        cases[k].fn(&cv, 0.0f, 0.0f);
        int intact = guards_intact();

        canvas_t big;
        canvas_setup(&big, bigbuf, BW, BH);
        memset(bigbuf, 0, sizeof bigbuf);
        cases[k].fn(&big, (float)W, (float)H);

        int worst = 0, drawn = 0;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int d = abs(green(px(x, y)) - green(bigbuf[(y + H) * BW + x + W]));
                if (d > worst) worst = d;
                if (px(x, y)) drawn++;
            }
        }
        char what[96];
        snprintf(what, sizeof what, "clipped %s matches the unclipped one (worst %d/63)",
                 cases[k].name, worst);
        expect(what, worst <= 1 && drawn > 0 && intact);
    }
}

static void test_off_canvas(void)
{
    /* Entirely off: nothing drawn. */
    reset(0);
    vec_disc(&cv, -50.0f, 100.0f, 49.0f, 0xFFFF);
    vec_ring(&cv, 400.0f, 100.0f, 50.0f, 10.0f, 0xFFFF);
    vec_arc(&cv, 120.0f, -80.0f, 70.0f, 10.0f, 2.0f, 4.2f, 0xFFFF);
    vec_line(&cv, -10.0f, -10.0f, 300.0f, -10.0f, 10.0f, 0xFFFF);
    vec_line(&cv, 250.0f, 0.0f, 250.0f, 280.0f, 10.0f, 0xFFFF);
    static const float tri[] = { -100, -100, -5, -100, -50, 400 };
    vec_polygon(&cv, tri, 3, 0xFFFF);
    expect("off-canvas primitives draw nothing", count_not(0) == 0);
    expect("off-canvas: guards intact", guards_intact());

    /* A line to a point 1e30 away is the same as one to a point on the same
       ray just off the panel. */
    reset(0);
    vec_line(&cv, 100.0f, 100.0f, 1e30f, 1e30f, 3.0f, 0xFFFF);
    static uint16_t first[W * H];
    memcpy(first, cv.fb, sizeof first);
    reset(0);
    vec_line(&cv, 100.0f, 100.0f, 1000.0f, 1000.0f, 3.0f, 0xFFFF);
    int diff = 0;
    for (int i = 0; i < W * H; i++) if (abs(green(cv.fb[i]) - green(first[i])) > 1) diff++;
    expect("line to 1e30 draws as the line towards it", diff == 0 && count_not(0) > 0);

    /* So is a polygon with a vertex out there. */
    reset(0);
    static const float far_tri[] = { 50, 50, 1e30f, 50, 50, 1e30f };
    vec_polygon(&cv, far_tri, 3, 0xFFFF);
    memcpy(first, cv.fb, sizeof first);
    reset(0);
    static const float near_tri[] = { 50, 50, 10000, 50, 50, 10000 };
    vec_polygon(&cv, near_tri, 3, 0xFFFF);
    expect("polygon to 1e30 draws as the one towards it", memcmp(first, cv.fb, sizeof first) == 0);
    expect("... which covers the corner beyond (50, 50)",
           px(60, 60) == 0xFFFF && px(239, 279) == 0xFFFF && px(49, 60) == 0);
}

/* Throws everything unreasonable at every primitive. Nothing may land outside
   the framebuffer, and non-finite arguments must draw nothing at all. */
static void test_hostile(void)
{
    static const float nasty[] = { NAN, INFINITY, -INFINITY };
    int nothing = 1;
    for (size_t k = 0; k < sizeof nasty / sizeof nasty[0]; k++) {
        float v = nasty[k];
        reset(0);
        vec_disc(&cv, v, 100, 20, 0xFFFF);
        vec_disc(&cv, 100, v, 20, 0xFFFF);
        vec_disc(&cv, 100, 100, v, 0xFFFF);
        vec_ring(&cv, v, 100, 20, 3, 0xFFFF);
        vec_ring(&cv, 100, 100, 20, v, 0xFFFF);
        vec_arc(&cv, 100, 100, 20, 3, v, 1, 0xFFFF);
        vec_arc(&cv, 100, 100, 20, 3, 0, v, 0xFFFF);
        vec_arc_butt(&cv, 100, 100, v, 3, 0, 1, 0xFFFF);
        vec_line(&cv, v, 10, 100, 100, 3, 0xFFFF);
        vec_line(&cv, 10, 10, 100, v, 3, 0xFFFF);
        vec_line(&cv, 10, 10, 100, 100, v, 0xFFFF);
        float p[] = { 10, 10, 100, 10, v, 100 };
        vec_polygon(&cv, p, 3, 0xFFFF);
        if (count_not(0) != 0) nothing = 0;
        if (!guards_intact()) nothing = 0;
    }
    expect("NaN and infinite arguments draw nothing", nothing);

    static const float huge[] = { 1e7f, -1e7f, 1e9f, -1e9f, 1e30f, -1e30f,
                                  FLT_MAX, -FLT_MAX, 3e38f, -3e38f, 1e-40f };
    int n = (int)(sizeof huge / sizeof huge[0]);
    int intact = 1;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            float a = huge[i], b = huge[j];
            reset(0);
            vec_disc(&cv, a, b, 30, 0xFFFF);
            vec_disc(&cv, 120, 140, a, 0xFFFF);
            vec_ring(&cv, a, 140, b, 5, 0xFFFF);
            vec_ring(&cv, 120, 140, 50, a, 0xFFFF);
            vec_arc(&cv, 120, 140, 60, 5, a, b, 0xFFFF);
            vec_arc_butt(&cv, 120, 140, 60, 5, a, b, 0xFFFF);
            vec_arc(&cv, a, b, 60, 5, 0, 1, 0xFFFF);
            vec_line(&cv, a, b, 120, 140, 4, 0xFFFF);
            vec_line(&cv, a, b, b, a, 4, 0xFFFF);
            vec_line(&cv, a, a, b, b, 2, 0xFFFF);
            vec_line(&cv, 10, 10, 100, 100, a, 0xFFFF);
            float p1[] = { a, b, 120, 140, b, a };
            vec_polygon(&cv, p1, 3, 0xFFFF);
            float p2[] = { a, a, b, a, b, b, a, b };
            vec_polygon(&cv, p2, 4, 0xFFFF);
            float p3[] = { a, 10, 200, b, 20, 250, b, a };
            vec_polygon(&cv, p3, 4, 0xFFFF);
            if (!guards_intact()) {
                printf("     guards hit with %g, %g\n", a, b);
                intact = 0;
            }
        }
    }
    expect("huge and tiny coordinates never write outside the framebuffer", intact);

    /* A polygon reaching across float's whole range still fills what it
       covers: this one holds the whole panel. */
    reset(0);
    static const float all[] = { -3e38f, -3e38f, 3e38f, -3e38f, 3e38f, 3e38f, -3e38f, 3e38f };
    vec_polygon(&cv, all, 4, 0xFFFF);
    expect("polygon at +-3e38 fills the panel", count_not(0xFFFF) == 0 && guards_intact());

    /* A null or empty canvas is refused. */
    vec_disc(NULL, 10, 10, 5, 0xFFFF);
    canvas_t empty;
    canvas_setup(&empty, NULL, W, H);
    vec_line(&empty, 0, 0, 100, 100, 3, 0xFFFF);
    canvas_setup(&empty, buf + GUARD, 0, 0);
    vec_polygon(&empty, all, 4, 0xFFFF);
    expect("null or empty canvas: refused without a crash", guards_intact());
}

/* ---- a face, for eyes and for a rough cost --------------------------------- */

static void draw_face(canvas_t *c)
{
    const float cx = 120, cy = 140;
    for (int i = 0; i < W * H; i++) c->fb[i] = 0x10A2;
    vec_disc(c, cx, cy, 116, 0x2124);
    vec_ring(c, cx, cy, 112, 1.5f, 0xEF5B);
    for (int m = 0; m < 60; m++) {
        float a = m * PI_F / 30, s = sinf(a), co = cosf(a);
        float r0 = m % 5 ? 106 : 94;
        vec_line(c, cx + r0 * s, cy - r0 * co, cx + 110 * s, cy - 110 * co,
                 m % 5 ? 1.0f : 3.0f, 0xEF5B);
    }
    vec_ring(c, cx, cy + 55, 26, 1.2f, 0xEF5B);
    vec_arc_butt(c, cx - 55, cy, 26, 4, -2.2f, 2.2f, 0x4228);
    vec_arc_butt(c, cx - 55, cy, 26, 4, -2.2f, 0.9f, 0x07E0);
    vec_arc(c, cx + 55, cy, 26, 3, 0.0f, 4.0f, 0xFD20);
    /* hour and minute hands at 10:09, seconds at 36 */
    float hour[] = { 0, -60, 7, -30, 3, 10, -3, 10, -7, -30 };
    float minute[] = { 0, -100, 5, -50, 2.5f, 14, -2.5f, 14, -5, -50 };
    float angles[2] = { (10 + 9 / 60.0f) * PI_F / 6, 9 * PI_F / 30 };
    float *shapes[2] = { hour, minute };
    for (int h = 0; h < 2; h++) {
        float s = sinf(angles[h]), co = cosf(angles[h]), p[10];
        for (int i = 0; i < 5; i++) {
            float x = shapes[h][2 * i], y = shapes[h][2 * i + 1];
            p[2 * i] = cx + x * co - y * s;
            p[2 * i + 1] = cy + x * s + y * co;
        }
        vec_polygon(c, p, 5, 0xE6A0);
    }
    float sa = 36 * PI_F / 30;
    vec_line(c, cx, cy + 55, cx + 22 * sinf(sa), cy + 55 - 22 * cosf(sa), 1.0f, 0xF800);
    vec_disc(c, cx, cy, 5, 0xE6A0);
    vec_disc(c, cx, cy, 2, 0x2124);
}

static void write_ppm(const char *path, const canvas_t *c)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) { printf("     cannot write %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", c->w, c->h);
    for (int i = 0; i < c->w * c->h; i++) {
        uint16_t v = c->fb[i];
        unsigned char rgb[3] = { (unsigned char)(((v >> 11) & 31) * 255 / 31),
                                 (unsigned char)(((v >> 5) & 63) * 255 / 63),
                                 (unsigned char)((v & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("     wrote %s\n", path);
}

static void test_face(void)
{
    reset(0);
    clock_t t0 = clock();
    int frames = 200;
    for (int i = 0; i < frames; i++) draw_face(&cv);
    double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC / frames;
    printf("     a sample face: %.3f ms a frame on this host\n", ms);
    expect("sample face: guards intact", guards_intact());

    /* VEC_PPM=face.ppm ./test_vector writes it out to look at. */
    const char *path = getenv("VEC_PPM");
    if (path != NULL && *path != '\0') write_ppm(path, &cv);
}

int main(void)
{
    test_blend();
    test_blend_linear();
    test_convention();
    test_disc();
    test_ring();
    test_arc();
    test_line();
    test_polygon();
    test_polygon_strips();
    test_clip_exact();
    test_off_canvas();
    test_hostile();
    test_face();

    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("PASS test_vector\n");
    return 0;
}
