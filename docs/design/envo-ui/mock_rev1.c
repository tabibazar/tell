/* Mockups of the proposed envo environment pages, drawn with the firmware's
   real canvas.c (12x24 bitmap font), vector.c and vfont.c onto 320x172.
   Scratch only: nothing here is in the repository. */
#include "canvas.h"
#include "vector.h"
#include "vfont.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 320
#define H 172
static uint16_t fb[W * H];
static canvas_t c1, c2;   /* same framebuffer, bitmap font at 1x and 2x */

/* ---- palette (proposal) ---- */
#define BG     0x0000
#define FG     0xFFFF
#define GREY   0x8410   /* labels, 5.5:1 */
#define RULE   0x5ACB   /* band edges, gridlines, 3.0:1 */
#define BLUE   0x04BF   /* OK      6.8:1 */
#define AMBER  0xFD40   /* AIR IT  11:1 */
#define RED    0xF8C1   /* ACT, only ever as a filled block with black text */
#define T_OK   0x00E7   /* band tints: navy */
#define T_FAIR 0x3920   /*             dark amber */
#define T_POOR 0x61E0   /*             brighter amber-brown */
#define HATCH  0x5ACB
#define ENVL   0x4A69   /* min-max envelope */
#define T_OKCELL 0x0149  /* week cell, OK: quiet blue (0,40,74) */

enum { S_WAIT, S_OK, S_FAIR, S_ACT };
static uint16_t state_col(int s) { return s == S_OK ? BLUE : s == S_FAIR ? AMBER : s == S_ACT ? RED : GREY; }

/* ---- text helpers.  y is always the TOP of the capitals' ink. ---- */
/* bitmap 12x24: capitals' ink occupies rows 4..17 of the cell */
static void t1(int x, int ytop, const char *s, uint16_t col) { canvas_puts_px(&c1, x, ytop - 4, s, col); }
static void t2(int x, int ytop, const char *s, uint16_t col) { canvas_puts_px(&c2, x, ytop - 8, s, col); }
static int  w1(const char *s) { return 12 * (int)strlen(s); }
static int  w2(const char *s) { return 24 * (int)strlen(s); }
/* vector font: size = cap height px */
static float vw(float size, float wt, const char *s) { vfont_style_t st = { size, wt, 0 }; return vfont_width(&st, s); }
static void vt(float x, float ytop, float size, float wt, int align, uint16_t col, const char *s)
{ vfont_style_t st = { size, wt, 0 }; vfont_draw(&c1, &st, x, ytop + size / 2, 0, align, col, s); }
static void vtt(float x, float ytop, float size, float wt, float track, int align, uint16_t col, const char *s)
{ vfont_style_t st = { size, wt, track }; vfont_draw(&c1, &st, x, ytop + size / 2, 0, align, col, s); }

static void rect(int x, int y, int w, int h, uint16_t col) { canvas_fill_rect(&c1, x, y, w, h, col); }
static void hline(int x0, int x1, int y, uint16_t col, int dotted)
{ for (int x = x0; x <= x1; x++) if (!dotted || (x & 3) < 2) rect(x, y, 1, 1, col); }
static void vline(int x, int y0, int y1, uint16_t col, int dotted)
{ for (int y = y0; y <= y1; y++) if (!dotted || (y & 3) < 2) rect(x, y, 1, 1, col); }
static void hatch(int x0, int y0, int x1, int y1)
{ for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) if (((x + y) % 6) == 0) rect(x, y, 1, 1, HATCH); }

/* degree sign after a vfont number: ring of radius size*0.12 */
static void degree(float x, float ytop, float size, uint16_t col)
{ float r = size * 0.11f; vec_ring(&c1, x + r + 1, ytop + r + 0.5f, r, size * 0.07f + 0.6f, col); }

/* trend arrow, a filled polygon. dir: +2 fast up, +1 up, 0 steady, -1 down, -2 fast down.
   (cx, cy) is the centre of an s x s box. */
static void arrow(float cx, float cy, float s, int dir, uint16_t col)
{
    if (dir == 0) { /* steady: a flat arrow pointing right */
        float xy[] = { cx - s/2, cy - s*0.12f, cx + s*0.1f, cy - s*0.12f, cx + s*0.1f, cy - s*0.35f,
                       cx + s/2, cy, cx + s*0.1f, cy + s*0.35f, cx + s*0.1f, cy + s*0.12f, cx - s/2, cy + s*0.12f };
        vec_polygon(&c1, xy, 7, col); return;
    }
    float sg = dir > 0 ? -1.f : 1.f;   /* up is negative y */
    float xy[] = { cx - s*0.12f, cy - sg*s/2, cx - s*0.12f, cy + sg*s*0.02f, cx - s*0.38f, cy + sg*s*0.02f,
                   cx, cy + sg*s/2, cx + s*0.38f, cy + sg*s*0.02f, cx + s*0.12f, cy + sg*s*0.02f, cx + s*0.12f, cy - sg*s/2 };
    vec_polygon(&c1, xy, 7, col);
    if (dir == 2 || dir == -2) { /* second head for fast */
        float o = sg * s * 0.42f;
        float xy2[] = { cx - s*0.38f, cy + sg*s*0.02f - o, cx, cy + sg*s/2 - o, cx + s*0.38f, cy + sg*s*0.02f - o,
                        cx + s*0.38f, cy + sg*s*0.02f - o - sg*3, cx, cy + sg*s/2 - o - sg*3, cx - s*0.38f, cy + sg*s*0.02f - o - sg*3 };
        (void)xy2;
    }
}

/* ---- data: 24 h at 5 min, now = Fri 14:30 ---- */
#define N 288
static float voc[N], co2[N], tc[N], rh[N];
static int   valid[N];   /* 0 ok, 1 warm-up, -1 no data (off) */
static float rnd(void) { return (float)rand() / RAND_MAX * 2 - 1; }
static int   idx_min(int i) { return (14 * 60 + 30) - (N - 1 - i) * 5; } /* minutes rel. to Fri 00:00 */
static void make_data(float now_voc)
{
    srand(7);
    for (int i = 0; i < N; i++) {
        int m = idx_min(i);                 /* negative = Thursday */
        float v = 40 + 7 * rnd();
        float dt = (float)(m - (-24 * 60 + 18 * 60 + 15));      /* dinner Thu 18:15 */
        if (dt >= 0) v += 380 * expf(-dt / 55.f);
        dt = (float)(m - (12 * 60 + 30));                        /* lunch Fri 12:30 */
        if (dt >= 0) v += 190 * expf(-dt / 40.f);
        voc[i] = v;
        valid[i] = 0;
        if (m >= 3 * 60 + 10 && m < 6 * 60) valid[i] = -1;       /* power bank flat */
        else if (m >= 6 * 60 && m < 6 * 60 + 5) valid[i] = 1;     /* warm-up */
        float occ = (m > -24*60 + 17*60 && m < 60) || (m > 7*60 && m < 9*60) || (m > 12*60) ? 1 : 0;
        co2[i] = 400 + 0.85f * (v - 30) + occ * 60 + 10 * rnd();
        if (co2[i] < 400) co2[i] = 400;
        float h = fmodf((float)m / 60.f + 48, 24);
        tc[i] = 24.9f + 1.3f * sinf((h - 9) / 24 * 6.2832f) + 0.08f * rnd();
        rh[i] = 44 - 4 * sinf((h - 9) / 24 * 6.2832f) + 0.5f * rnd() + ((h > 7.2f && h < 7.6f) ? 8 : 0);
    }
    if (now_voc > 0) {   /* bad air now: ramp the last 40 min */
        for (int k = 0; k < 8; k++) {
            int i = N - 1 - k;
            voc[i] = now_voc * (1 - k * 0.11f);
            co2[i] = 400 + 0.85f * (voc[i] - 30) + 60;
        }
    }
}

/* ---- band ladder (equal-height bands, linear inside each) ---- */
static const float VOC_E[] = { 0, 65, 220, 650, 2200, 5500 };
static const float CO2_E[] = { 400, 600, 800, 1000, 1500, 2000 };
static float ladder(const float *e, float v)   /* 0..5 */
{
    if (v <= e[0]) return 0;
    for (int b = 0; b < 5; b++) if (v < e[b + 1]) return b + (v - e[b]) / (e[b + 1] - e[b]);
    return 5;
}
static int band_state(int b) { return b <= 1 ? S_OK : b == 2 ? S_FAIR : S_ACT; }
static uint16_t band_tint(int b) { return b <= 1 ? T_OK : b == 2 ? T_FAIR : T_POOR; }
static int voc_state(float v) { return band_state((int)fminf(4, floorf(ladder(VOC_E, v)))); }
static int co2_state(float v) { return band_state((int)fminf(4, floorf(ladder(CO2_E, v)))); }
static const char *WORD5[] = { "GOOD", "GOOD", "FAIR", "POOR", "BAD" };
static const char *voc_word(float v) { return WORD5[(int)fminf(4, floorf(ladder(VOC_E, v)))]; }
static const char *co2_word(float v) { return WORD5[(int)fminf(4, floorf(ladder(CO2_E, v)))]; }

/* status word: OK = blue text, FAIR = amber text, ACT = red filled box, black text */
static int status_word(float x, float ytop, float size, float wt, int align, int st, const char *s)
{
    float w = vw(size, wt, s);
    float x0 = align < 0 ? x : align > 0 ? x - w : x - w / 2;
    if (st == S_ACT) {
        int pad = (int)(size * 0.35f + 0.5f);
        rect((int)(x0 - pad), (int)(ytop - pad), (int)(w + 2 * pad + 0.5f), (int)(size + 2 * pad + 0.5f), RED);
        vt(x0, ytop, size, wt, VFONT_LEFT, BG, s);
    } else vt(x0, ytop, size, wt, VFONT_LEFT, state_col(st), s);
    return (int)(w + 0.5f);
}

/* ---- a 24 h band-ladder chart ----
   plot box x0..x1 (inclusive), y0..y1; nb bands of equal height from the bottom. */
static void ladder_chart(int x0, int y0, int x1, int y1, const float *e, const float *d, int labels, int big)
{
    int nb = 5, ph = y1 - y0 + 1, pw = x1 - x0 + 1;
    float bh = (float)ph / nb;
    /* only the OK zone (bands 0-1) is tinted: Tufte's normal band */
    { int ya = y1 + 1 - (int)lroundf(2 * bh); rect(x0, ya, pw, y1 + 1 - ya, T_OK); }
    if (big) for (int b = 1; b < nb; b++) {
        int y = y1 + 1 - (int)lroundf(b * bh);
        hline(x0, x1, y, b == 2 ? AMBER : b == 3 ? RED : RULE, b == 1 || b == 4);
    } else hline(x0, x1, y1 + 1 - (int)lroundf(3 * bh), RULE, 1);
    /* per-pixel columns */
    float lastx = -1, lasty = 0; int have = 0;
    int peak_i = -1; float peak_v = -1;
    for (int px = 0; px < pw; px++) {
        int i0 = px * N / pw, i1 = (px + 1) * N / pw; if (i1 <= i0) i1 = i0 + 1;
        float lo = 1e9, hi = -1e9, sum = 0; int n = 0, gap = 0;
        for (int i = i0; i < i1; i++) {
            if (valid[i] != 0) { gap = 1; continue; }
            lo = fminf(lo, d[i]); hi = fmaxf(hi, d[i]); sum += d[i]; n++;
            if (d[i] > peak_v) { peak_v = d[i]; peak_i = px; }
        }
        int x = x0 + px;
        if (n == 0) { if (gap) hatch(x, y0, x, y1); have = 0; continue; }
        float ylo = y1 + 1 - ladder(e, lo) * bh, yhi = y1 + 1 - ladder(e, hi) * bh;
        if (big && ylo - yhi >= 1.5f) vline(x, (int)yhi, (int)ylo, ENVL, 0);
        float ym = y1 + 1 - ladder(e, sum / n) * bh;
        float lv = ladder(e, sum / n);
        uint16_t tc_ = lv < 2 ? FG : lv < 3 ? AMBER : RED;
        if (have) vec_line(&c1, lastx, lasty, x + 0.5f, ym, big ? 2.0f : 1.6f, tc_);
        lastx = x + 0.5f; lasty = ym; have = 1;
    }
    /* now dot */
    vec_disc(&c1, lastx, lasty, big ? 3.5f : 2.5f, FG);
    if (labels && peak_i >= 0 && x0 + peak_i < lastx - 16) {   /* mark the day's peak, unless it is now */
        float yp = y1 + 1 - ladder(e, peak_v) * bh;
        float px = x0 + peak_i + 0.5f;
        float tri[] = { px - 4, yp - 9, px + 4, yp - 9, px, yp - 3 };
        vec_polygon(&c1, tri, 3, FG);
        char b[16]; snprintf(b, sizeof b, "%.0f", peak_v);
        int lx = (int)px + 7; if (lx + w1(b) > x1) lx = (int)px - 7 - w1(b);
        t1(lx, (int)yp - 14, b, FG);
    }
}

static void time_axis(int x0, int x1, int ytop)
{
    int pw = x1 - x0 + 1;
    /* hour marks at 18, 00 (weekday), 06, 12; NOW at right */
    const int marks[] = { -6 * 60, 0, 6 * 60, 12 * 60 };
    const char *lab[] = { "18", "FRI", "06", "12" };
    for (int k = 0; k < 4; k++) {
        float f = (float)(marks[k] - idx_min(0)) / (float)(idx_min(N - 1) - idx_min(0));
        int x = x0 + (int)lroundf(f * (pw - 1));
        if (k == 1) vline(x, ytop - 104, ytop - 4, GREY, 0);    /* midnight: solid */
        else vline(x, ytop - 4, ytop - 2, GREY, 0);
        if (x + w1(lab[k]) / 2 < x1 - w1("NOW") - 2) t1(x - w1(lab[k]) / 2, ytop, lab[k], k == 1 ? FG : GREY);
    }
    t1(x1 - w1("NOW") + 4, ytop, "NOW", FG);
}

/* ---------------- pages ---------------- */

/* HOME: AIR NOW */
static void page_home(float v, float c2v, int varr, int carr, int warm_s)
{
    canvas_clear(&c1);
    int vs = voc_state(v), cs = co2_state(c2v);
    if (warm_s > 0) vs = cs = S_WAIT;
    /* divider */
    vline(160, 8, 138, RULE, 0);
    const char *lab[2] = { "VOC", "eCO2 est" };
    const char *unit[2] = { "ppb", "ppm" };
    float val[2] = { v, c2v };
    int st[2] = { vs, cs }, ar[2] = { varr, carr };
    const float *edges[2] = { VOC_E, CO2_E };
    const float *series[2] = { voc, co2 };
    for (int k = 0; k < 2; k++) {
        int x = k ? 168 : 14, xr = k ? 308 : 148;
        t1(x, 8, lab[k], GREY);
        t1(xr - w1(unit[k]), 8, unit[k], GREY);
        char b[16];
        if (warm_s > 0) snprintf(b, sizeof b, "--"); else snprintf(b, sizeof b, "%.0f", val[k]);
        float size = 46; while (vw(size, 6, b) > (xr - x) && size > 30) size -= 2;
        vt(x, 30, size, 6, VFONT_LEFT, warm_s > 0 ? GREY : FG, b);
        /* word + arrow */
        if (warm_s > 0) {
            vt(x, 88, 18, 3, VFONT_LEFT, GREY, "WARM-UP");
        } else {
            const char *w = k ? co2_word(val[k]) : voc_word(val[k]);
            status_word(x + 1, 86, 22, 3.4f, VFONT_LEFT, st[k], w);
            arrow(xr - 10, 86 + 11, 20, ar[k], FG);
        }
        /* sparkline: last 24 h */
        ladder_chart(x, 120, xr, 140, edges[k], series[k], 0, 0);
        if (warm_s > 0) { /* dim it */
            for (int yy = 120; yy <= 140; yy++) for (int xx = x; xx <= xr; xx++) {
                uint16_t *p = &fb[yy * W + xx]; *p = vec_blend(*p, BG, 150); }
        }
    }
    /* footer / advice row, y 146..171 */
    int worst = vs > cs ? vs : cs;
    if (warm_s > 0) {
        char b[32]; snprintf(b, sizeof b, "SENSOR WARMING UP %d:%02d", warm_s / 60, warm_s % 60);
        t1(160 - w1(b) / 2, 153, b, GREY);
    } else if (worst == S_ACT) {
        rect(0, 145, W, 27, RED);
        vt(160, 150, 16, 3, VFONT_CENTRE, BG, "OPEN A WINDOW NOW");
    } else if (worst == S_FAIR) {
        vt(16, 151, 14, 2.4f, VFONT_LEFT, AMBER, "AIR THE ROOM SOON");
        t1(304 - w1("14:32"), 151, "14:32", GREY);
    } else {
        /* room climate + time */
        vt(16, 151, 14, 2.4f, VFONT_LEFT, FG, "26.2");
        float wx = 16 + vw(14, 2.4f, "26.2") + 2; degree(wx, 151, 14, FG);
        vt(wx + 8, 151, 14, 2.4f, VFONT_LEFT, FG, "C");
        vt(wx + 28, 151, 14, 2.4f, VFONT_LEFT, FG, "43%");
        t1(304 - w1("14:32"), 151, "14:32", GREY);
    }
}

/* VOC / CO2E detail, 24 h */
static void page_gas(int co2page, float now, int arr)
{
    canvas_clear(&c1);
    const float *e = co2page ? CO2_E : VOC_E;
    int st = co2page ? co2_state(now) : voc_state(now);
    const char *w = co2page ? co2_word(now) : voc_word(now);
    /* header y 0..40 */
    char b[16]; snprintf(b, sizeof b, "%.0f", now);
    vt(16, 7, 30, 4.5f, VFONT_LEFT, FG, b);
    int nx = 16 + (int)vw(30, 4.5f, b) + 8;
    t1(nx, 7, co2page ? "eCO2" : "VOC", FG);
    t1(nx, 23, co2page ? "est." : "ppb", GREY);
    int ww = status_word(304 - 24, 9, 22, 3.4f, VFONT_RIGHT, st, w);
    (void)ww;
    arrow(304 - 8, 9 + 11, 18, arr, FG);
    /* plot */
    int x0 = 54, x1 = 266, y0 = 44, y1 = 143;
    ladder_chart(x0, y0, x1, y1, e, co2page ? co2 : voc, 1, 1);
    /* left gutter: edge values, right gutter: words */
    float bh = (y1 - y0 + 1) / 5.f;
    const char *ev_voc[] = { "65", "220", "650", "2200" };
    const char *ev_co2[] = { "600", "800", "1000", "1500" };
    for (int k = 1; k < 5; k++) {
        const char *s = co2page ? ev_co2[k - 1] : ev_voc[k - 1];
        int y = y1 + 1 - (int)lroundf(k * bh);
        t1(x0 - 4 - w1(s), y - 7, s, GREY);
    }
    t1(x0 - 4 - w1("ppm"), 152, co2page ? "ppm" : "ppb", GREY);
    const char *bw[] = { "GOOD", "FAIR", "POOR", "BAD" };
    const float mid[] = { 1.0f, 2.5f, 3.5f, 4.5f };
    for (int k = 0; k < 4; k++) {
        int y = y1 + 1 - (int)lroundf(mid[k] * bh);
        t1(270, y - 7, bw[k], k == 0 ? BLUE : k == 1 ? AMBER : RED);
    }
    time_axis(x0, x1, 150);
}

/* ROOM: temperature and humidity */
static void mini_linear(int x0, int y0, int x1, int y1, float lo, float hi, float blo, float bhi,
                        const float *d, const float *ticks, int nt, const char *fmt)
{
    int ph = y1 - y0 + 1, pw = x1 - x0 + 1;
#define YV(v) (y1 + 1 - ((v) - lo) / (hi - lo) * ph)
    rect(x0, (int)lroundf(YV(bhi)), pw, (int)lroundf(YV(blo) - YV(bhi)), T_OK);
    for (int k = 0; k < nt; k++) {
        int y = (int)lroundf(YV(ticks[k]));
        hline(x0, x1, y, RULE, 1);
        char b[8]; snprintf(b, sizeof b, fmt, ticks[k]);
        t1(x1 + 4, y - 7, b, GREY);
    }
    float lx = 0, ly = 0; int have = 0;
    for (int px = 0; px < pw; px++) {
        int i0 = px * N / pw, i1 = (px + 1) * N / pw; if (i1 <= i0) i1 = i0 + 1;
        float s = 0; int n = 0, gap = 0;
        for (int i = i0; i < i1; i++) { if (valid[i] < 0) { gap = 1; continue; } s += d[i]; n++; }
        if (n == 0) { if (gap) hatch(x0 + px, y0, x0 + px, y1); have = 0; continue; }
        float y = YV(s / n);
        if (have) vec_line(&c1, lx, ly, x0 + px + 0.5f, y, 2.0f, FG);
        lx = x0 + px + 0.5f; ly = y; have = 1;
    }
    vec_disc(&c1, lx, ly, 3, FG);
#undef YV
}

static void page_room(int calibrated)
{
    canvas_clear(&c1);
    float dT = calibrated ? 3.0f : 0.0f;
    static float tcc[N], rhc[N];
    for (int i = 0; i < N; i++) { /* vapour-pressure correction */
        float es0 = 6.112f * expf(17.62f * tc[i] / (243.12f + tc[i]));
        float t = tc[i] - dT, es1 = 6.112f * expf(17.62f * t / (243.12f + t));
        tcc[i] = t; rhc[i] = rh[i] * es0 / es1; }
    /* row 1: temperature, y 4..80 */
    t1(16, 6, calibrated ? "TEMP" : "TEMP ~raw", GREY);
    char tb[16]; snprintf(tb, sizeof tb, "%.1f", tcc[N-1]);
    vt(16, 26, 30, 4.5f, VFONT_LEFT, FG, tb);
    float dx = 16 + vw(30, 4.5f, tb) + 4; degree(dx, 26, 30, FG);
    vt(dx + 12, 26, 14, 2.4f, VFONT_LEFT, GREY, "C");
    if (calibrated) status_word(16, 64, 14, 2.4f, VFONT_LEFT, tcc[N-1] > 25 ? S_FAIR : S_OK, tcc[N-1] > 25 ? "WARM" : "COMFORTABLE");
    else t1(16, 64, "reads ~3C high", GREY);
    const float tt[] = { 20, 25 };
    mini_linear(132, 8, 276, 76, 16, 30, calibrated ? 20 : 0, calibrated ? 25 : 0, tcc, tt, 2, "%.0f");
    /* row 2: humidity, y 86..160 */
    hline(16, 304, 84, RULE, 0);
    t1(16, 92, calibrated ? "HUMIDITY" : "RH ~raw", GREY);
    char hb[16]; snprintf(hb, sizeof hb, "%.0f%%", rhc[N-1]);
    vt(16, 112, 30, 4.5f, VFONT_LEFT, FG, hb);
    if (calibrated) status_word(16, 150, 14, 2.4f, VFONT_LEFT, S_OK, "GOOD");
    else t1(16, 150, "reads low", GREY);
    const float rt[] = { 30, 60 };
    mini_linear(132, 92, 276, 150, 20, 80, calibrated ? 30 : 0, calibrated ? 60 : 0, rhc, rt, 2, "%.0f");
    t1(132, 155, "-24H", GREY);
    t1(276 - w1("NOW"), 155, "NOW", FG);
}

/* WEEK: worst air state held >= 15 min in each hour, 7 days x 24 h */
static void page_week(void)
{
    canvas_clear(&c1);
    t1(16, 8, "AIR WEEK", GREY);
    vt(304, 8, 14, 2.4f, VFONT_RIGHT, FG, "3 OF 5 DAYS GOOD");
    const char *dn[] = { "SA", "SU", "MO", "TU", "WE", "TH", "FR" };
    int gx = 44, gy = 30, cw = 11, ch = 16;
    for (int d = 0; d < 7; d++) {
        t1(14, gy + d * ch + 1, dn[d], d == 6 ? FG : GREY);
        for (int h = 0; h < 24; h++) {
            int x = gx + h * cw, y = gy + d * ch;
            int s = S_OK;
            if (d == 6 && h > 14) { /* rest of today: outline only */
                hline(x, x + cw - 2, y, 0x2945, 0); hline(x, x + cw - 2, y + ch - 3, 0x2945, 0);
                vline(x, y, y + ch - 3, 0x2945, 0); vline(x + cw - 2, y, y + ch - 3, 0x2945, 0); continue; }
            if ((d == 2 || d == 3) && (h < 17)) { rect(x + cw/2 - 1, y + ch/2 - 2, 2, 2, RULE); continue; } /* no data */
            if (d == 6 && h >= 3 && h < 6) { rect(x + cw/2 - 1, y + ch/2 - 2, 2, 2, RULE); continue; }
            if (h >= 18 && h <= 20 && d != 1) s = S_FAIR;
            if (d == 5 && (h == 18 || h == 19)) s = S_ACT;
            if (d == 6 && h == 12) s = S_FAIR;
            if (d == 0 && h == 11) s = S_ACT;
            uint16_t col = s == S_OK ? T_OKCELL : s == S_FAIR ? AMBER : RED;
            rect(x, y, cw - 1, ch - 2, col);
            if (s == S_ACT) rect(x + cw/2 - 2, y + ch/2 - 3, 4, 4, BG);   /* hole: shape cue */
        }
    }
    const int hs[] = { 0, 6, 12, 18 };
    for (int k = 0; k < 4; k++) {
        char b[4]; snprintf(b, sizeof b, "%02d", hs[k]);
        vline(gx + hs[k] * cw, gy + 7 * ch - 1, gy + 7 * ch + 1, GREY, 0);
        t1(gx + hs[k] * cw, gy + 7 * ch + 4, b, GREY);
    }
    t1(gx + 24 * cw - w1("24"), gy + 7 * ch + 4, "24", GREY);
}


/* Approach A: one reading per page, Aranet-style */
static void page_single(const char *name, const char *unit, float v, const float *e, const float *d, int arr)
{
    canvas_clear(&c1);
    int st = voc_state(v); const char *w = voc_word(v);
    t1(16, 8, name, GREY); t1(304 - w1(unit), 8, unit, GREY);
    char b[16]; snprintf(b, sizeof b, "%.0f", v);
    vt(16, 28, 64, 8, VFONT_LEFT, FG, b);
    arrow(304 - 14, 28 + 16, 28, arr, FG);
    vt(304, 66, 14, 2.4f, VFONT_RIGHT, GREY, "STEADY");
    status_word(16, 102, 24, 3.6f, VFONT_LEFT, st, w);
    ladder_chart(16, 134, 304, 152, e, d, 0, 0);
    t1(16, 156, "-24H", GREY); t1(304 - w1("NOW"), 156, "NOW", GREY);
    for (int k = 0; k < 6; k++) { if (k == 0) vec_disc(&c1, 136 + k * 10, 162.5f, 2.5f, FG); else vec_ring(&c1, 136 + k * 10, 162.5f, 2.2f, 1.0f, RULE); }
}
/* Approach B: repaired clock home */
static void page_clockhome(void)
{
    canvas_clear(&c1);
    t1(160 - w1("Fri 25 Sep") / 2, 8, "Fri 25 Sep", GREY);
    vt(160, 30, 56, 7, VFONT_CENTRE, FG, "14:32");
    /* air line: word first */
    int x = 16;
    x += status_word(x, 112, 18, 3, VFONT_LEFT, S_OK, "AIR GOOD") + 12;
    t1(x, 114, "VOC 48", FG); x += w1("VOC 48") + 12;
    t1(x, 114, "eCO2 441", FG);
    t1(16, 146, "ROOM 26.2C 43%", GREY);
}
/* ---------------- output ---------------- */
static void put16(FILE *f, unsigned v) { fputc((int)(v & 0xFF), f); fputc((int)((v >> 8) & 0xFF), f); }
static void put32(FILE *f, unsigned long v) { put16(f, (unsigned)(v & 0xFFFF)); put16(f, (unsigned)((v >> 16) & 0xFFFF)); }
static void write_bmp(const char *path, int zoom)
{
    FILE *f = fopen(path, "wb"); assert(f);
    int ow = W * zoom, oh = H * zoom, stride = (ow * 3 + 3) & ~3;
    fputc('B', f); fputc('M', f); put32(f, 54ul + (unsigned long)stride * oh); put32(f, 0); put32(f, 54);
    put32(f, 40); put32(f, ow); put32(f, oh); put16(f, 1); put16(f, 24); put32(f, 0);
    put32(f, (unsigned long)stride * oh); put32(f, 2835); put32(f, 2835); put32(f, 0); put32(f, 0);
    for (int y = oh - 1; y >= 0; y--) {
        int n = 0;
        for (int x = 0; x < ow; x++) {
            uint16_t p = fb[(y / zoom) * W + x / zoom];
            unsigned r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
            fputc((int)((b << 3) | (b >> 2)), f); fputc((int)((g << 2) | (g >> 4)), f); fputc((int)((r << 3) | (r >> 2)), f);
            n += 3;
        }
        while (n < stride) { fputc(0, f); n++; }
    }
    fclose(f);
}
static void emit(const char *name)
{
    char p[128];
    snprintf(p, sizeof p, "out/%s.bmp", name); write_bmp(p, 1);
    snprintf(p, sizeof p, "out/%s_3x.bmp", name); write_bmp(p, 3);
    printf("%s\n", name);
}

int main(void)
{
    canvas_init(&c1, fb, W, H, 1);
    canvas_init(&c2, fb, W, H, 2);
    /* measurements for the spec */
    printf("vfont widths: 1366@46w6=%.1f 1554@46=%.1f 48@46=%.1f 456@46=%.1f 5500@46=%.1f\n",
           vw(46, 6, "1366"), vw(46, 6, "1554"), vw(46, 6, "48"), vw(46, 6, "456"), vw(46, 6, "5500"));
    printf("words@18w3: EXCELLENT=%.1f GOOD=%.1f FAIR=%.1f POOR=%.1f BAD=%.1f\n",
           vw(18, 3, "EXCELLENT"), vw(18, 3, "GOOD"), vw(18, 3, "FAIR"), vw(18, 3, "POOR"), vw(18, 3, "BAD"));
    printf("footer@16w3 OPEN A WINDOW NOW=%.1f  @14 AIR THE ROOM SOON=%.1f 5 OF 7 DAYS GOOD=%.1f\n",
           vw(16, 3, "OPEN A WINDOW NOW"), vw(14, 2.4f, "AIR THE ROOM SOON"), vw(14, 2.4f, "5 OF 7 DAYS GOOD"));
    printf("30@4.5: 1366=%.1f 23.2=%.1f 48%%=%.1f\n", vw(30, 4.5f, "1366"), vw(30, 4.5f, "23.2"), vw(30, 4.5f, "48%"));

    make_data(0);
    voc[N-1] = 48; co2[N-1] = 441;
    page_home(48, 441, 0, 0, 0);          emit("m1_home_good");
    page_gas(0, 48, 0);                   emit("m2_voc_day");
    page_gas(1, 441, 0);                  emit("m3_co2_day");
    page_room(1);                         emit("m4_room");
    page_room(0);                         emit("m4b_room_uncalibrated");
    page_week();                          emit("m5_week");
    make_data(0);
    voc[N-1] = 260; co2[N-1] = 612;
    page_home(260, 612, 1, 1, 0);         emit("m6_home_fair");
    make_data(1366);
    page_home(1366, 1554, 2, 2, 0);       emit("m7_home_act");
    page_gas(0, 1366, 2);                 emit("m8_voc_day_act");
    make_data(0);
    page_home(0, 400, 0, 0, 134);         emit("m9_home_warmup");
    make_data(0);
    voc[N-1] = 48;
    page_single("VOC", "ppb", 48, VOC_E, voc, 0); emit("a1_single_voc");
    page_clockhome();                     emit("b1_clock_home");
    return 0;
}
