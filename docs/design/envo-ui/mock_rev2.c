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

/* ================= v2 (revised after critique) =================
   The eCO2 series above is INVENTED (400 + 0.85*(VOC-30) + 60*occupancy): it is
   there to fill a page, not a model of the ENS160. */

/* three states, three words; only the two state edges are drawn */
static const float VOC_L[] = { 0, 65, 220, 650, 2200 };      /* ladder, 4 equal bands */
static const float CO2_L[] = { 400, 600, 800, 1000, 1500 };
static float lad4(const float *e, float v)                    /* 0..4, pinned at the top */
{
    if (v <= e[0]) return 0;
    for (int b = 0; b < 4; b++) if (v < e[b + 1]) return b + (v - e[b]) / (e[b + 1] - e[b]);
    return 4;
}
static int st_of(const float *e, float v) { return v < e[2] ? S_OK : v < e[3] ? S_FAIR : S_ACT; }
static const char *WORD3[] = { "--", "GOOD", "FAIR", "POOR" };

/* display rounding: VOC 5 ppb < 100, 10 to 1000, 100 above; eCO2 10 ppm */
static float round_voc(float v) { float q = v < 100 ? 5 : v < 1000 ? 10 : 100; return q * floorf(v / q); }   /* truncate: never shows an edge the reading has not reached */
static float round_co2(float v) { return 10 * floorf(v / 10); }

/* state word: OK blue text, FAIR amber text, ACT black on a red block */
static int sword(float x, float ytop, float size, int align, int st, const char *s)
{
    float wt = size * 0.15f, w = vw(size, wt, s);
    float x0 = align < 0 ? x : align > 0 ? x - w : x - w / 2;
    if (st == S_ACT) {
        int pad = (int)(size * 0.2f + 0.5f);
        rect((int)(x0 - pad), (int)(ytop - pad), (int)(w + 2 * pad + 0.5f), (int)(size + 2 * pad + 0.5f), RED);
        vt(x0, ytop, size, wt, VFONT_LEFT, BG, s);
    } else vt(x0, ytop, size, wt, VFONT_LEFT, st == S_WAIT ? GREY : state_col(st), s);
    return (int)(w + 0.5f);
}
/* single trend arrow, up or down only; nothing when steady */
static void arrow1(float cx, float cy, float s, int dir, uint16_t col)
{
    if (dir == 0) return;
    float sg = dir > 0 ? -1.f : 1.f;
    float xy[] = { cx - s*0.13f, cy - sg*s/2, cx - s*0.13f, cy + sg*s*0.02f, cx - s*0.4f, cy + sg*s*0.02f,
                   cx, cy + sg*s/2, cx + s*0.4f, cy + sg*s*0.02f, cx + s*0.13f, cy + sg*s*0.02f, cx + s*0.13f, cy - sg*s/2 };
    vec_polygon(&c1, xy, 7, col);
}
/* text on a black knock-out */
static void t1k(int x, int ytop, const char *s, uint16_t col) { rect(x - 2, ytop - 2, w1(s) + 4, 18, BG); t1(x, ytop, s, col); }

static void temp_rh(float tc_, float rh_, float x, float ytop, float size, uint16_t col, float *xend)
{
    char b[16]; snprintf(b, sizeof b, "%.1f", tc_);
    vt(x, ytop, size, size * 0.15f, VFONT_LEFT, col, b);
    float dx = x + vw(size, size * 0.15f, b) + 2; degree(dx, ytop, size, col);
    float cx = dx + size * 0.3f + 2;
    vt(cx, ytop, size, size * 0.15f, VFONT_LEFT, col, "C");
    float hx = cx + vw(size, size * 0.15f, "C") + size * 0.6f;
    snprintf(b, sizeof b, "%.0f%%", rh_);
    vt(hx, ytop, size, size * 0.15f, VFONT_LEFT, col, b);
    if (xend) *xend = hx + vw(size, size * 0.15f, b);
}

static float cur_t(void) { return tc[N - 1] - 3.0f; }
static float cur_rh(void) { float t0 = tc[N - 1], t1_ = t0 - 3.0f;
    return rh[N - 1] * expf(17.62f * t0 / (243.12f + t0)) / expf(17.62f * t1_ / (243.12f + t1_)); }
/* ---- AIR NOW (home) ---- */
static void page_air(float v, float c, int dir, int warm_min, const char *tag)
{
    canvas_clear(&c1);
    t1(16, 8, "AIR NOW", GREY);
    t1(304 - w1("14:32"), 8, "14:32", GREY);
    float rv = round_voc(v), rc = round_co2(c);
    if (warm_min >= 0) {
        sword(16, 28, 34, VFONT_LEFT, S_WAIT, "WARMING UP");
        t1(16, 74, "TEMP est", GREY);
        t1(304 - w1("HUMIDITY est"), 74, "HUMIDITY est", GREY);
        char b[16]; snprintf(b, sizeof b, "%.1f", cur_t());
        vt(16, 94, 46, 6, VFONT_LEFT, FG, b);
        float dx = 16 + vw(46, 6, b) + 3; degree(dx, 94, 46, FG);
        t1((int)dx + 16, 96, "C", GREY);
        { char hb[8]; snprintf(hb, sizeof hb, "%.0f%%", cur_rh()); vt(304, 94, 46, 6, VFONT_RIGHT, FG, hb); }
        char m[24]; snprintf(m, sizeof m, "%d MIN SO FAR", warm_min);
        vt(16, 148, 22, 3.3f, VFONT_LEFT, GREY, m);
        (void)tag; return;
    }
    int vs = st_of(VOC_L, v), cs = st_of(CO2_L, c);   /* state from the unrounded value */
    int worst = vs > cs ? vs : cs;
    const char *who = worst == S_OK ? "AIR" : (cs > vs ? "ECO2" : "VOC");
    /* verdict row, ink 28..62 */
    vt(16, 28, 34, 5.1f, VFONT_LEFT, FG, who);
    float wx = 16 + vw(34, 5.1f, who) + 20;
    sword(wx, 28, 34, VFONT_LEFT, worst, WORD3[worst]);
    /* label row */
    t1(16, 74, "VOC ppb", GREY);
    t1(304 - w1("eCO2 est ppm"), 74, "eCO2 est ppm", GREY);
    /* numbers, bottoms aligned at 140 */
    char b[16]; snprintf(b, sizeof b, "%.0f", rv);
    float size = 46; while (vw(size, 6, b) > 190 && size > 34) size -= 2;
    vt(16, 140 - size, size, size * 0.13f, VFONT_LEFT, FG, b);
    arrow1(16 + vw(size, 6, b) + 20, 140 - size / 2, 24, dir, FG);
    snprintf(b, sizeof b, "%.0f", rc);
    vt(304, 110, 30, 4.5f, VFONT_RIGHT, FG, b);
    /* bottom row, ink 148..170 */
    if (worst == S_ACT) {
        rect(0, 143, W, 29, RED);
        vt(160, 148, 22, 3.3f, VFONT_CENTRE, BG, "VENTILATE NOW");
    } else if (worst == S_FAIR) {
        vt(16, 148, 22, 3.3f, VFONT_LEFT, AMBER, "VENTILATE SOON");
    } else {
        t1(16, 152, "ROOM", GREY);
        float xe; temp_rh(cur_t(), cur_rh(), 70, 148, 22, FG, &xe);
        t1((int)xe + 8, 152, "est", GREY);
    }
    (void)tag;
}

/* ---- VOC 24H / eCO2 24H ---- */
static float zh(const float *e, const float *zb, float v)
{
    if (v <= e[0]) return 0;
    for (int b = 0; b < 4; b++) if (v < e[b + 1]) return zb[b] + (v - e[b]) / (e[b + 1] - e[b]) * (zb[b + 1] - zb[b]);
    return zb[4];
}
static int idx_x(int i, int x0, int pw) { return x0 + (int)((long)i * pw / N); }
static void page_chart(int co2page, float now_raw, int dir)
{
    canvas_clear(&c1);
    const float *e = co2page ? CO2_L : VOC_L;
    const float *d = co2page ? co2 : voc;
    float now = co2page ? round_co2(now_raw) : round_voc(now_raw);
    int st = st_of(e, now_raw);
    /* title slot, fixed on every page */
    t1(16, 8, co2page ? "eCO2 est 24H" : "VOC 24H", FG);
    t1(16, 24, co2page ? "ppm, from VOCs" : "ppb", GREY);
    /* value + word, right */
    int wx = 304;
    if (!(co2page && st == S_OK)) wx -= sword(304, 12, 22, VFONT_RIGHT, st, WORD3[st]) + 10;
    if (st == S_ACT) wx -= 4;
    if (dir) { arrow1(wx - 9, 23, 18, dir, FG); wx -= 24; }
    char b[16]; snprintf(b, sizeof b, "%.0f", now);
    float nsz = 30; int title_r = 16 + w1(co2page ? "eCO2 est 24H" : "VOC 24H") + 12;
    while (wx - vw(nsz, 4.5f, b) < title_r && nsz > 22) nsz -= 2;
    printf("chart header: number %s at size %.0f, left x %.0f (title ends %d)\n", b, nsz, wx - vw(nsz, 4.5f, b), title_r);
    vt(wx, 8 + (30 - nsz), nsz, nsz * 0.15f, VFONT_RIGHT, FG, b);
    /* plot: zones of fixed height -- GOOD 42 px (0-65-220), FAIR 32, POOR 24 -- linear inside each */
    int x0 = 16, x1 = 249, y0 = 44, y1 = 141, pw = x1 - x0 + 1;
    /* three zones, each linear: no hidden breakpoint inside GOOD */
    const float zb[] = { 0, 42.f * (e[1] - e[0]) / (e[2] - e[0]), 42, 74, 98 };
#define YL(v) (y1 + 1 - zh(e, zb, (v)))
    if (!co2page) { int ya = (int)lroundf(YL(e[2])); rect(x0, ya, pw, y1 + 1 - ya, T_OK); }
    int yf = (int)lroundf(YL(e[2])), yp = (int)lroundf(YL(e[3]));
    hline(x0, x1, yf, AMBER, 0); hline(x0, x1, yp, RED, 0);
    hline(x0, x1, y1 + 1, RULE, 0);   /* floor */
    /* key in the right gutter, aligned to the lines: POOR / 650 / FAIR / 220 / GOOD */
    t1(256, y0 + 1, "POOR", RED);
    t1(256, yp - 7, co2page ? "1000" : "650", GREY);
    t1(256, (yf + yp) / 2 - 7, "FAIR", AMBER);
    t1(256, yf - 7, co2page ? "800" : "220", GREY);
    if (!co2page) t1(256, yf + 14, "GOOD", BLUE);
    /* trace: column means; gaps hatched */
    float lx = -1, ly = 0; int have = 0, pk_px = -1; float pk_v = -1;
    for (int px = 0; px < pw; px++) {
        int i0 = px * N / pw, i1 = (px + 1) * N / pw; if (i1 <= i0) i1 = i0 + 1;
        float s = 0; int n = 0, gap = 0;
        for (int i = i0; i < i1; i++) { if (valid[i] != 0) { gap = 1; continue; } s += d[i]; n++; if (d[i] > pk_v) { pk_v = d[i]; pk_px = px; } }
        if (n == 0) { if (gap) hatch(x0 + px, y0, x0 + px, y1); have = 0; continue; }
        float m = s / n, y = YL(m);
        uint16_t col = st_of(e, m) == S_OK ? FG : st_of(e, m) == S_FAIR ? AMBER : RED;
        if (have) vec_line(&c1, lx, ly, x0 + px + 0.5f, y, 2.0f, col);
        lx = x0 + px + 0.5f; ly = y; have = 1;
    }
    /* now dot = the display value, not the last column */
    float ny = YL(now_raw);
    { int sn = st_of(e, now_raw); vec_line(&c1, lx, ly, x1 + 0.5f, ny, 2.0f, sn == S_OK ? FG : sn == S_FAIR ? AMBER : RED); }
    vec_disc(&c1, x1 + 0.5f, ny, 3.5f, FG);
    /* peak: only if it reached FAIR, and not next to now */
    if (pk_px >= 0 && st_of(e, pk_v) >= S_FAIR && x0 + pk_px < x1 - 20) {
        float px = x0 + pk_px + 0.5f, py = YL(pk_v);
        float tri[] = { px - 4, py - 9, px + 4, py - 9, px, py - 3 };
        vec_polygon(&c1, tri, 3, FG);
        char pb[16]; snprintf(pb, sizeof pb, "%.0f", co2page ? round_co2(pk_v) : round_voc(pk_v));
        int lx2 = (int)px + 7; if (lx2 + w1(pb) > x1) lx2 = (int)px - 7 - w1(pb);
        int ly2 = (int)py - 16; if (ly2 < y0) ly2 = y0;
        t1k(lx2, ly2, pb, FG);
    }
#undef YL
    /* time axis */
    const int marks[] = { -6 * 60, 0, 6 * 60, 12 * 60 };
    const char *lab[] = { "18", "FRI", "06", "12" };
    for (int k = 0; k < 4; k++) {
        float f = (float)(marks[k] - idx_min(0)) / (float)(idx_min(N - 1) - idx_min(0));
        int x = x0 + (int)lroundf(f * (pw - 1));
        if (k == 1) vline(x, y0, y1, GREY, 0); else vline(x, y1 + 2, y1 + 4, GREY, 0);
        if (x + w1(lab[k]) / 2 < x1 - w1("NOW") - 4) t1(x - w1(lab[k]) / 2, 150, lab[k], k == 1 ? FG : GREY);
    }
    t1(x1 - w1("NOW") + 1, 150, "NOW", FG);
}

/* ---- ROOM ---- */
static void mini(int x0, int y0, int x1, int y1, float lo, float hi, const float *ref, const float *dd)
{
    int ph = y1 - y0 + 1, pw = x1 - x0 + 1;
#define YV(v) (y1 + 1 - ((v) - lo) / (hi - lo) * ph)
    for (int k = 0; k < 2; k++) { int y = (int)lroundf(YV(ref[k])); hline(x0, x1, y, RULE, 0);
        char b[8]; snprintf(b, sizeof b, "%.0f", ref[k]); t1(x1 + 6, y - 7, b, GREY); }
    float lx = 0, ly = 0; int have = 0;
    for (int px = 0; px < pw; px++) {
        int i0 = px * N / pw, i1 = (px + 1) * N / pw; if (i1 <= i0) i1 = i0 + 1;
        float s = 0; int n = 0, gap = 0;
        for (int i = i0; i < i1; i++) { if (valid[i] < 0) { gap = 1; continue; } s += dd[i]; n++; }
        if (n == 0) { if (gap) hatch(x0 + px, y0, x0 + px, y1); have = 0; continue; }
        float y = YV(s / n);
        if (have) vec_line(&c1, lx, ly, x0 + px + 0.5f, y, 2.0f, FG);
        lx = x0 + px + 0.5f; ly = y; have = 1;
    }
    vec_disc(&c1, lx, ly, 3, FG);
#undef YV
}
static void page_room2(void)
{
    canvas_clear(&c1);
    static float tcc[N], rhc[N];
    for (int i = 0; i < N; i++) {
        float es0 = 6.112f * expf(17.62f * tc[i] / (243.12f + tc[i]));
        float t = tc[i] - 3.0f, es1 = 6.112f * expf(17.62f * t / (243.12f + t));
        tcc[i] = t; rhc[i] = rh[i] * es0 / es1; }
    t1(16, 8, "ROOM 24H", FG);
    t1(16 + w1("ROOM 24H") + 12, 8, "est, -3.0C", GREY);
    /* temperature */
    t1(16, 30, "TEMP", GREY);
    char b[16]; snprintf(b, sizeof b, "%.1f", tcc[N - 1]);
    vt(16, 48, 30, 4.5f, VFONT_LEFT, FG, b);
    float dx = 16 + vw(30, 4.5f, b) + 3; degree(dx, 48, 30, FG);
    t1((int)dx + 12, 50, "C", GREY);
    const float tr[] = { 20, 25 };
    mini(132, 30, 276, 82, 16, 30, tr, tcc);
    hline(16, 304, 90, RULE, 0);
    t1(16, 98, "HUMIDITY", GREY);
    snprintf(b, sizeof b, "%.0f%%", rhc[N - 1]);
    vt(16, 116, 30, 4.5f, VFONT_LEFT, FG, b);
    const float rr[] = { 30, 60 };
    mini(132, 98, 276, 146, 20, 80, rr, rhc);
    t1(132, 152, "-24H", GREY);
    t1(276 - w1("NOW"), 152, "NOW", FG);
}

/* ---- AIR WEEK + 30 days: ink for exceptions, height = how bad ---- */
static void cell(int x, int y, int w, int h, int s)
{
    if (s == S_WAIT) { rect(x + w / 2 - 1, y + h / 2 - 1, 2, 2, RULE); return; }   /* no data */
    int hh = s == S_OK ? 3 : s == S_FAIR ? (h + 1) / 2 : h;
    rect(x, y + h - hh, w, hh, state_col(s));
}
static void page_week2(void)
{
    canvas_clear(&c1);
    t1(16, 8, "WEEK", FG);
    /* takeaway at 1x: a label for what the grid already shows */
    int nf = 0, np = 0;
    const char *dn[] = { "SA", "SU", "MO", "TU", "WE", "TH", "FR" };
    int gx = 44, gy = 30, cw = 11, ch = 17;
    for (int dd = 0; dd < 7; dd++) {
        t1(16, gy + dd * ch, dn[dd], dd == 6 ? FG : GREY);
        for (int h = 0; h < 24; h++) {
            int cx = gx + h * cw, cy = gy + dd * ch;
            if (dd == 6 && h > 14) continue;                                   /* still to come: nothing */
            int s2 = S_OK;
            if ((dd == 2 || dd == 3) && h < 17) s2 = S_WAIT;
            if (dd == 6 && h >= 3 && h < 6) s2 = S_WAIT;
            if (s2 != S_WAIT) {
                if (h >= 18 && h <= 20 && dd != 1) s2 = S_FAIR;
                if (dd == 5 && (h == 18 || h == 19)) s2 = S_ACT;
                if (dd == 6 && h == 12) s2 = S_FAIR;
                if (dd == 0 && h == 11) s2 = S_ACT;
            }
            cell(cx, cy + 1, cw - 2, ch - 4, s2);
            nf += s2 == S_FAIR; np += s2 == S_ACT;
        }
    }
    { char fb_[16], pb_[16]; snprintf(fb_, sizeof fb_, "FAIR %dH", nf); snprintf(pb_, sizeof pb_, "POOR %dH", np);
      int x = 304 - w1(fb_); t1(x, 8, fb_, AMBER); x -= 24 + w1(pb_); t1(x, 8, pb_, RED); }
    const int hs[] = { 0, 6, 12, 18 };
    for (int k = 0; k < 4; k++) {
        char b2[4]; snprintf(b2, sizeof b2, "%02d", hs[k]);
        t1(gx + hs[k] * cw - (k ? 12 : 0), gy + 7 * ch + 2, b2, GREY);
    }
    t1(gx + 24 * cw - 2 - w1("24"), gy + 7 * ch + 2, "24", GREY);
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
    printf("widths: ECO2 FAIR@34=%.0f AIR GOOD@34=%.0f VOC POOR@34=%.0f WARMING UP@34=%.0f VENTILATE SOON@22=%.0f VENTILATE NOW@22=%.0f 12 MIN SO FAR@22=%.0f GAS ERROR@34=%.0f\n",
           vw(34, 5.1f, "ECO2 FAIR"), vw(34, 5.1f, "AIR GOOD"), vw(34, 5.1f, "VOC POOR"), vw(34, 5.1f, "WARMING UP"),
           vw(22, 3.3f, "VENTILATE SOON"), vw(22, 3.3f, "VENTILATE NOW"), vw(22, 3.3f, "12 MIN SO FAR"), vw(34, 5.1f, "GAS ERROR"));
    printf("numbers@46: 45=%.0f 260=%.0f 1400=%.0f 12000=%.0f  eCO2@30: 440=%.0f 1550=%.0f\n",
           vw(46, 6, "45"), vw(46, 6, "260"), vw(46, 6, "1400"), vw(46, 6, "12000"), vw(30, 4.5f, "440"), vw(30, 4.5f, "1550"));
    make_data(0); voc[N-1] = 45; co2[N-1] = 441;
    page_air(47, 441, 0, -1, "good");      emit("v1_air_good");
    page_chart(0, 47, 0);                  emit("v2_voc_24h");
    page_chart(1, 441, 0);                 emit("v3_eco2_24h");
    page_room2();                          emit("v4_room");
    page_week2();                          emit("v5_week");
    make_data(0); voc[N-1] = 262; co2[N-1] = 612;
    page_air(262, 612, 1, -1, "fair");     emit("v6_air_voc_fair");
    make_data(0); voc[N-1] = 180; co2[N-1] = 870;
    page_air(180, 870, 0, -1, "co2");      emit("v7_air_eco2_fair");
    make_data(1366);
    page_air(1366, 1554, 1, -1, "act");    emit("v8_air_poor");
    page_chart(0, 1366, 1);                emit("v9_voc_24h_poor");
    make_data(0);
    page_air(0, 400, 0, 12, "warm");       emit("v10_air_warmup");
    return 0;
}
