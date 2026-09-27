#include "dirui.h"

#include "aafont.h"
#include "noiseui.h"
#include "vector.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the estimate ----------------------------------------------------------- */

#define SOUND_MS    343.0f
#define F_LO        300.0f
#define F_HI        4500.0f
#define TAU_STEP    0.04f           /* samples between candidate delays */
#define TAU_SPAN    1.8f            /* search +-this many samples: the 1.6 there is, and some */
#define N_TAU       91              /* 2 x TAU_SPAN / TAU_STEP + 1 */

/* The FFTs' buffers and the candidate delays' rotations, allocated once as
   one block (on speaker it lands in PSRAM, leaving internal RAM to BLE). */
typedef struct {
    float re1[DIRUI_N], im1[DIRUI_N], re2[DIRUI_N], im2[DIRUI_N];
    float win[DIRUI_N], cs[DIRUI_N / 2], sn[DIRUI_N / 2];
    int   k0, k1;                                   /* the bins in F_LO..F_HI */
    float rot_c[N_TAU][DIRUI_N / 2], rot_s[N_TAU][DIRUI_N / 2];
    float fs;
} dir_mem_t;
static dir_mem_t *s_d;

static bool setup(float fs)
{
    if (s_d && s_d->fs == fs) return true;
    if (!s_d) s_d = malloc(sizeof *s_d);
    if (!s_d) return false;
    const float pi = 3.14159265358979f;
    for (int i = 0; i < DIRUI_N; i++) s_d->win[i] = 0.5f - 0.5f * cosf(2.0f * pi * (float)i / (float)DIRUI_N);
    for (int i = 0; i < DIRUI_N / 2; i++) {
        s_d->cs[i] = cosf(2.0f * pi * (float)i / (float)DIRUI_N);
        s_d->sn[i] = -sinf(2.0f * pi * (float)i / (float)DIRUI_N);
    }
    s_d->k0 = (int)ceilf(F_LO * DIRUI_N / fs);
    s_d->k1 = (int)floorf(F_HI * DIRUI_N / fs);
    if (s_d->k1 > DIRUI_N / 2 - 1) s_d->k1 = DIRUI_N / 2 - 1;
    for (int t = 0; t < N_TAU; t++) {
        float tau = -TAU_SPAN + (float)t * TAU_STEP;
        for (int k = 0; k < DIRUI_N / 2; k++) {
            float w = 2.0f * pi * (float)k / (float)DIRUI_N;
            s_d->rot_c[t][k] = cosf(w * tau);
            s_d->rot_s[t][k] = sinf(w * tau);
        }
    }
    s_d->fs = fs;
    return true;
}

static void fft(float *re, float *im)
{
    const int n = DIRUI_N;
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < len / 2; k++) {
                float wr = s_d->cs[k * step], wi = s_d->sn[k * step];
                float *ar = &re[i + k], *ai = &im[i + k], *br = &re[i + k + len / 2], *bi = &im[i + k + len / 2];
                float tr = *br * wr - *bi * wi, ti = *br * wi + *bi * wr;
                *br = *ar - tr; *bi = *ai - ti;
                *ar += tr;      *ai += ti;
            }
        }
    }
}

bool dirui_estimate(const int16_t *mic1, const int16_t *mic2, float fs, float *deg, float *confidence)
{
    if (!setup(fs)) return false;
    double e = 0;
    for (int i = 0; i < DIRUI_N; i++) {
        s_d->re1[i] = (float)mic1[i] * s_d->win[i];
        s_d->re2[i] = (float)mic2[i] * s_d->win[i];
        s_d->im1[i] = s_d->im2[i] = 0.0f;
        e += (double)mic1[i] * mic1[i] + (double)mic2[i] * mic2[i];
    }
    if (e < 1.0) return false;                    /* digital silence */
    fft(s_d->re1, s_d->im1);
    fft(s_d->re2, s_d->im2);

    /* The phase transform: X1 conj(X2) / |X1 conj(X2)|, kept in re1/im1. */
    int nb = 0;
    for (int k = s_d->k0; k <= s_d->k1; k++) {
        float cr = s_d->re1[k] * s_d->re2[k] + s_d->im1[k] * s_d->im2[k];
        float ci = s_d->im1[k] * s_d->re2[k] - s_d->re1[k] * s_d->im2[k];
        float m = sqrtf(cr * cr + ci * ci);
        if (m > 1e-9f) { s_d->re1[k] = cr / m; s_d->im1[k] = ci / m; nb++; }
        else { s_d->re1[k] = s_d->im1[k] = 0.0f; }
    }
    if (nb == 0) return false;

    /*
     * R(tau) = sum Re(G e^{+j w tau}). MIC2 hearing it L samples after MIC1
     * puts e^{-j w L} into X2, so G's phase is +w L and R peaks at tau = -L.
     * Then a parabola through the best and its neighbours for the fraction
     * between candidates.
     */
    float best = -1e30f, r[N_TAU];
    int bt = 0;
    for (int t = 0; t < N_TAU; t++) {
        float acc = 0.0f;
        for (int k = s_d->k0; k <= s_d->k1; k++)
            acc += s_d->re1[k] * s_d->rot_c[t][k] - s_d->im1[k] * s_d->rot_s[t][k];
        r[t] = acc;
        if (acc > best) { best = acc; bt = t; }
    }
    float frac = 0.0f;
    if (bt > 0 && bt < N_TAU - 1) {
        float a = r[bt - 1], b = r[bt], c2 = r[bt + 1], den = a - 2.0f * b + c2;
        if (den < 0.0f) frac = 0.5f * (a - c2) / den;
    }
    float tau = TAU_SPAN - ((float)bt + frac) * TAU_STEP;       /* -(peak): samples MIC2 lags MIC1 */
    float tau_max = DIRUI_SPACING_M * fs / SOUND_MS;
    float x = tau / tau_max;
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    /* MIC2 later means MIC1 heard it first: the sound is at MIC1's end, negative. */
    *deg = -asinf(x) * 57.29578f;
    if (confidence) *confidence = best / (float)nb < 0.0f ? 0.0f : best / (float)nb;
    return true;
}

/* ---- the page ------------------------------------------------------------------ */

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define GROUND      RGB(6, 9, 22)
#define CREAM       RGB(236, 228, 206)
#define CREAM_DIM   RGB(140, 136, 122)
#define GOLD        RGB(224, 186, 118)
#define RAIL        RGB(52, 58, 78)

#define F_LABEL     (&aafont_inter_label)
#define F_SUB       (&aafont_inter_sub)
#define F_NUMBER    (&aafont_inter_number)

#define CX          120.0f
#define CY          160.0f
#define R_ARC       100.0f

/* A point on the dial: -90 left, 0 straight up, +90 right. */
static void at(float deg, float r, float *x, float *y)
{
    float a = deg * 0.01745329f;
    *x = CX + r * sinf(a);
    *y = CY - r * cosf(a);
}

void dirui_draw(canvas_t *c, const dirui_t *s)
{
    if (c == NULL || c->fb == NULL || c->w <= 0 || c->h <= 0) return;
    canvas_fill_rect(c, 0, 0, c->w, c->h, GROUND);
    if (s == NULL) return;
    bool was = vec_linear_light(true);

    aafont_draw(c, F_LABEL, 16, 12, "DIRECTION", GOLD, AAFONT_LEFT);
    if (s->have_signal && isfinite(s->laf)) {
        char b[16];
        snprintf(b, sizeof b, "%.0f dBA", (double)s->laf);
        aafont_draw(c, F_LABEL, c->w - 16, 12, b, CREAM_DIM, AAFONT_RIGHT);
    }

    /* The dial: the half circle, ticks every 30 degrees, the mics' ends. */
    vec_arc(c, CX, CY, R_ARC, 2.0f, -1.5708f, 1.5708f, RAIL);
    for (int d = -90; d <= 90; d += 30) {
        float x0, y0, x1, y1;
        at((float)d, R_ARC - 8.0f, &x0, &y0);
        at((float)d, R_ARC + 2.0f, &x1, &y1);
        vec_line(c, x0, y0, x1, y1, d == 0 ? 2.0f : 1.5f, RAIL);
    }
    canvas_fill_rect(c, (int)(CX - R_ARC) - 6, (int)CY, (int)(2 * R_ARC) + 12, 1, RAIL);
    aafont_draw(c, F_LABEL, 16, (int)CY + 8, "MIC 1", CREAM_DIM, AAFONT_LEFT);
    aafont_draw(c, F_LABEL, c->w - 16, (int)CY + 8, "MIC 2", CREAM_DIM, AAFONT_RIGHT);
    vec_disc(c, CX, CY, 4.0f, CREAM_DIM);

    /* The trail: recent bearings as dots on the arc, older fainter. */
    for (int i = 0; i < s->n_trail && i < DIRUI_TRAIL; i++) {
        float x, y;
        at(s->trail[i], R_ARC, &x, &y);
        float k = (float)(i + 1) / (float)s->n_trail;
        uint16_t col = noiseui_colour(isfinite(s->laf) ? s->laf : 40.0f);
        int r8 = (int)(((col >> 11) & 31) * k), g8 = (int)(((col >> 5) & 63) * k), b8 = (int)((col & 31) * k);
        vec_disc(c, x, y, 2.0f + 3.0f * k, (uint16_t)((r8 << 11) | (g8 << 5) | b8));
    }

    char b[32];
    if (!s->have_signal) {
        aafont_draw(c, F_SUB, (int)CX, 214, "no signal", CREAM_DIM, AAFONT_CENTRE);
    } else if (!s->active) {
        aafont_draw(c, F_SUB, (int)CX, 214, "listening", CREAM_DIM, AAFONT_CENTRE);
    } else {
        float x, y;
        at(s->deg, R_ARC - 14.0f, &x, &y);
        uint16_t col = noiseui_colour(s->laf);
        vec_line(c, CX, CY, x, y, 4.0f, col);
        vec_disc(c, x, y, 6.0f, col);
        int a = (int)lroundf(fabsf(s->deg));
        snprintf(b, sizeof b, "%d\xC2\xB0", a);
        aafont_draw(c, F_NUMBER, (int)CX, 196, b, CREAM, AAFONT_CENTRE);
        const char *side = a < 10 ? "straight ahead or behind" : s->deg < 0 ? "toward MIC 1" : "toward MIC 2";
        aafont_draw(c, F_LABEL, (int)CX, 196 + F_NUMBER->cap + 14, side, CREAM_DIM, AAFONT_CENTRE);
    }

    /* How sure: a thin bar along the foot. */
    aafont_draw(c, F_LABEL, 16, 300, "SURE", CREAM_DIM, AAFONT_LEFT);
    canvas_fill_rect(c, 64, 304, 160, 4, RAIL);
    if (s->active) {
        float k = s->confidence < 0.0f ? 0.0f : s->confidence > 1.0f ? 1.0f : s->confidence;
        canvas_fill_rect(c, 64, 304, (int)(160.0f * k), 4, GOLD);
    }
    vec_linear_light(was);
}
