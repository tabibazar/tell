#include "liveui.h"

#include "aafont.h"
#include "noiseui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Sound page's palette, on a flat ground: this page redraws twenty-odd
   times a second and has no time for the Sound page's dithered sky. */
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define GROUND      RGB(6, 9, 22)
#define CREAM       RGB(236, 228, 206)
#define CREAM_DIM   RGB(140, 136, 122)
#define GOLD        RGB(224, 186, 118)
#define RAIL        RGB(52, 58, 78)
#define TRACK       RGB(16, 20, 38)

#define F_LABEL     (&aafont_inter_label)   /* capitals 13 */
#define F_SUB       (&aafont_inter_sub)     /* 17 */

/* The waveform's box and the bars' rows. */
#define W_TOP       40
#define W_H         100
#define W_MID       (W_TOP + W_H / 2)
#define B_TOP       148
#define B_PITCH     19
#define B_BAR_H     12
#define B_X0        46                     /* bars start right of the labels */
#define B_X1        230
#define B_LO        20.0f
#define B_HI        90.0f

/* ---- the spectrum ---------------------------------------------------------- */

/* The FFT's workspace and tables, 20 KB, allocated on first use as one block
   rather than kept in .bss: on speaker a block that size goes to PSRAM
   (anything under 16 KB is kept internal), and internal RAM is what her
   Bluetooth stack needs -- as static arrays these left it too little to
   start. */
typedef struct {
    float re[LIVEUI_FFT], im[LIVEUI_FFT];
    float win[LIVEUI_FFT], cos[LIVEUI_FFT / 2], sin[LIVEUI_FFT / 2];
} fft_mem_t;
static fft_mem_t *s_m;
#define s_re  (s_m->re)
#define s_im  (s_m->im)
#define s_win (s_m->win)
#define s_cos (s_m->cos)
#define s_sin (s_m->sin)

static bool tables(void)
{
    s_m = malloc(sizeof *s_m);
    if (s_m == NULL) return false;
    const float pi = 3.14159265358979f;
    for (int i = 0; i < LIVEUI_FFT; i++)
        s_win[i] = 0.5f - 0.5f * cosf(2.0f * pi * (float)i / (float)LIVEUI_FFT);
    for (int i = 0; i < LIVEUI_FFT / 2; i++) {
        s_cos[i] = cosf(2.0f * pi * (float)i / (float)LIVEUI_FFT);
        s_sin[i] = -sinf(2.0f * pi * (float)i / (float)LIVEUI_FFT);
    }
    return true;
}

/* In place, radix 2, decimation in time. */
static void fft(void)
{
    const int n = LIVEUI_FFT;
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = s_re[i]; s_re[i] = s_re[j]; s_re[j] = t;
            t = s_im[i]; s_im[i] = s_im[j]; s_im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < len / 2; k++) {
                float wr = s_cos[k * step], wi = s_sin[k * step];
                float *ar = &s_re[i + k], *ai = &s_im[i + k];
                float *br = &s_re[i + k + len / 2], *bi = &s_im[i + k + len / 2];
                float tr = *br * wr - *bi * wi, ti = *br * wi + *bi * wr;
                *br = *ar - tr; *bi = *ai - ti;
                *ar += tr;      *ai += ti;
            }
        }
    }
}

void liveui_bands(const int16_t *x, float fs, float offset_db, float out_db[LIVEUI_BANDS])
{
    if (s_m == NULL && !tables()) {
        for (int b = 0; b < LIVEUI_BANDS; b++) out_db[b] = NAN;
        return;
    }
    for (int i = 0; i < LIVEUI_FFT; i++) {
        s_re[i] = (float)x[i] * (1.0f / 32768.0f) * s_win[i];
        s_im[i] = 0.0f;
    }
    fft();
    /* One-sided power per bin as a share of the mean square: 2|X|^2 / N^2,
       over the Hann window's power, 3/8. */
    const float norm = 2.0f / ((float)LIVEUI_FFT * (float)LIVEUI_FFT * 0.375f);
    const float hz = fs / (float)LIVEUI_FFT;
    for (int b = 0; b < LIVEUI_BANDS; b++) {
        float fc = 62.5f * (float)(1 << b);
        float lo = fc / 1.41421356f, hi = fc * 1.41421356f;
        double e = 0.0;
        for (int k = 1; k < LIVEUI_FFT / 2; k++) {
            float f = (float)k * hz;
            if (f >= lo && f < hi) e += (double)(s_re[k] * s_re[k] + s_im[k] * s_im[k]);
        }
        double ms = e * norm;
        out_db[b] = ms > 1e-12 ? (float)(10.0 * log10(ms)) + offset_db : -120.0f + offset_db;
    }
}

/* ---- the page ------------------------------------------------------------------ */

static const char *const s_band_label[LIVEUI_BANDS] = { "63", "125", "250", "500", "1k", "2k", "4k", "8k" };

static float bar_w(float db)
{
    float f = (db - B_LO) / (B_HI - B_LO);
    if (!(f > 0.0f)) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f * (float)(B_X1 - B_X0);
}

static void header(canvas_t *c, const liveui_t *s)
{
    aafont_draw(c, F_LABEL, 16, 12, "LIVE", GOLD, AAFONT_LEFT);
    if (!s->have_signal || !isfinite(s->laf)) {
        aafont_draw(c, F_LABEL, c->w - 16, 12, "NO SIGNAL", CREAM_DIM, AAFONT_RIGHT);
        return;
    }
    char b[16];
    snprintf(b, sizeof b, "%.0f", (double)s->laf);
    int uw = aafont_draw(c, F_LABEL, c->w - 16, 12, s->calibrated ? "dBA" : "dBA est", CREAM_DIM, AAFONT_RIGHT);
    aafont_draw(c, F_SUB, c->w - 16 - uw - 6, 8, b, noiseui_colour(s->laf), AAFONT_RIGHT);
}

/*
 * Two samples to a column, each column the envelope of its pair and a joint
 * with the next, so a fast wave reads as a band and a slow one as a line.
 * Scaled to the loudest sample shown, but never beyond 40x (a silent room's
 * hiss stays a flat line rather than filling the box).
 */
static void waveform(canvas_t *c, const liveui_t *s)
{
    for (int x = 16; x < c->w - 16; x += 4) canvas_fill_rect(c, x, W_MID, 2, 1, RAIL);
    if (!s->have_signal) return;
    int peak = 1;
    for (int i = 0; i < LIVEUI_WAVE; i++) {
        int v = s->wave[i] < 0 ? -s->wave[i] : s->wave[i];
        if (v > peak) peak = v;
    }
    float scale = (float)(W_H / 2 - 2) / (float)(peak * 40 < 32768 ? 32768 / 40 : peak);
    uint16_t col = noiseui_colour(s->laf);
    const int x0 = 16, cols = c->w - 32;
    float prev = 0.0f;
    for (int i = 0; i < cols; i++) {
        /* The samples under this column, and the last one before it, so
           neighbouring columns join. */
        int k0 = i * LIVEUI_WAVE / cols, k1 = (i + 1) * LIVEUI_WAVE / cols;
        float lo = i ? prev : (float)s->wave[k0] * scale, hi = lo;
        for (int k = k0; k < k1 && k < LIVEUI_WAVE; k++) {
            float v = (float)s->wave[k] * scale;
            lo = fminf(lo, v);
            hi = fmaxf(hi, v);
            prev = v;
        }
        int y0 = W_MID - (int)lroundf(hi), y1 = W_MID - (int)lroundf(lo);
        canvas_fill_rect(c, x0 + i, y0, 1, y1 - y0 + 1, col);
    }
}

static void bars(canvas_t *c, const liveui_t *s)
{
    for (int b = 0; b < LIVEUI_BANDS; b++) {
        int y = B_TOP + b * B_PITCH;
        aafont_draw(c, F_LABEL, B_X0 - 8, y + (B_BAR_H - F_LABEL->cap) / 2, s_band_label[b], CREAM_DIM,
                    AAFONT_RIGHT);
        canvas_fill_rect(c, B_X0, y, B_X1 - B_X0, B_BAR_H, TRACK);
        float v = s->band[b];
        if (s->have_signal && isfinite(v)) {
            int w = (int)lroundf(bar_w(v));
            if (w > 0) canvas_fill_rect(c, B_X0, y, w, B_BAR_H, noiseui_colour(v));
        }
        float p = s->peak[b];
        if (s->have_signal && isfinite(p) && p > B_LO) {
            int px = B_X0 + (int)lroundf(bar_w(p));
            if (px > B_X1 - 2) px = B_X1 - 2;
            canvas_fill_rect(c, px, y - 1, 2, B_BAR_H + 2, CREAM);
        }
    }
    /* The scale under the bars: its ends and the middle, in dB. */
    int ly = B_TOP + LIVEUI_BANDS * B_PITCH;
    aafont_draw(c, F_LABEL, B_X0, ly, "20", CREAM_DIM, AAFONT_LEFT);
    aafont_draw(c, F_LABEL, (B_X0 + B_X1) / 2, ly, "55 dB", CREAM_DIM, AAFONT_CENTRE);
    aafont_draw(c, F_LABEL, B_X1, ly, "90", CREAM_DIM, AAFONT_RIGHT);
}

void liveui_draw(canvas_t *c, const liveui_t *s)
{
    if (c == NULL || c->fb == NULL || c->w <= 0 || c->h <= 0) return;
    canvas_fill_rect(c, 0, 0, c->w, c->h, GROUND);
    if (s == NULL) return;
    header(c, s);
    waveform(c, s);
    bars(c, s);
}
