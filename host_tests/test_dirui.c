/* dirui: GCC-PHAT finds a known fractional delay between the two mics, and
   the Direction page renders (host_tests/renders/dirui/, PPM). */
#include "dirui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int s_fail;
static void expect(const char *what, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) s_fail++;
}

#define LEN (DIRUI_N + 64)
static float s_src[LEN];

/* Band-limited noise, then each mic a windowed-sinc read of it at its own
   delay: `lag` samples MIC2 hears it after MIC1. */
static void make(int16_t *m1, int16_t *m2, float lag, unsigned seed, float noise2)
{
    srand(seed);
    for (int i = 0; i < LEN; i++) s_src[i] = (float)(rand() % 2001 - 1000);
    for (int pass = 0; pass < 2; pass++)          /* a gentle low-pass, so a sinc read is exact enough */
        for (int i = LEN - 1; i > 0; i--) s_src[i] = 0.6f * s_src[i] + 0.4f * s_src[i - 1];
    for (int i = 0; i < DIRUI_N; i++) {
        for (int m = 0; m < 2; m++) {
            float at = (float)(i + 32) - (m ? lag : 0.0f), acc = 0.0f;
            for (int k = -16; k <= 16; k++) {
                int j = (int)floorf(at) + k;
                if (j < 0 || j >= LEN) continue;
                float x = at - (float)j;
                float sinc = fabsf(x) < 1e-6f ? 1.0f : sinf(3.14159265f * x) / (3.14159265f * x);
                float w = 0.5f + 0.5f * cosf(3.14159265f * x / 17.0f);
                acc += s_src[j] * sinc * w;
            }
            if (m) acc += noise2 * (float)(rand() % 2001 - 1000);
            (m ? m2 : m1)[i] = (int16_t)acc;
        }
    }
}

static uint16_t s_fb[DIRUI_WIDTH * DIRUI_HEIGHT];
static void render(const char *name, const dirui_t *s)
{
    canvas_t c;
    canvas_init(&c, s_fb, DIRUI_WIDTH, DIRUI_HEIGHT, 1);
    dirui_draw(&c, s);
    mkdir("renders", 0755);
    mkdir("renders/dirui", 0755);
    char path[128];
    snprintf(path, sizeof path, "renders/dirui/%s.ppm", name);
    FILE *f = fopen(path, "wb");
    if (!f) { s_fail++; return; }
    fprintf(f, "P6 %d %d 255\n", DIRUI_WIDTH, DIRUI_HEIGHT);
    for (int i = 0; i < DIRUI_WIDTH * DIRUI_HEIGHT; i++) {
        uint16_t p = s_fb[i];
        unsigned char rgb[3] = { (unsigned char)((p >> 11) * 255 / 31), (unsigned char)(((p >> 5) & 63) * 255 / 63),
                                 (unsigned char)((p & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(void)
{
    static int16_t m1[DIRUI_N], m2[DIRUI_N];
    const float tau_max = DIRUI_SPACING_M * 16000.0f / 343.0f;
    const float lags[] = { 0.0f, 0.5f, 1.0f, -0.8f, 1.5f, -1.5f };
    for (size_t i = 0; i < sizeof lags / sizeof lags[0]; i++) {
        make(m1, m2, lags[i], 7 + (unsigned)i, 0.0f);
        float deg = 99, conf = 0;
        bool ok = dirui_estimate(m1, m2, 16000.0f, &deg, &conf);
        float want = -asinf(lags[i] / tau_max) * 57.29578f;
        printf("     lag %+.2f samples: %+.1f deg (want %+.1f), confidence %.2f\n", (double)lags[i], (double)deg,
               (double)want, (double)conf);
        char what[96];
        snprintf(what, sizeof what, "a %+.2f-sample lag reads within 5 degrees", (double)lags[i]);
        expect(what, ok && fabsf(deg - want) < 5.0f);
    }
    make(m1, m2, 0.0f, 99, 0.0f);
    float d, cf;
    dirui_estimate(m1, m2, 16000.0f, &d, &cf);
    float clear = cf;
    make(m1, m2, 0.0f, 99, 3.0f);
    dirui_estimate(m1, m2, 16000.0f, &d, &cf);
    printf("     confidence clean %.2f, with independent noise %.2f\n", (double)clear, (double)cf);
    expect("independent noise lowers the confidence", cf < clear);
    memset(m1, 0, sizeof m1);
    memset(m2, 0, sizeof m2);
    expect("silence is no estimate", !dirui_estimate(m1, m2, 16000.0f, &d, &cf));

    static dirui_t s;
    s.have_signal = true; s.active = true; s.laf = 62.0f; s.deg = -38.0f; s.confidence = 0.8f;
    for (int i = 0; i < DIRUI_TRAIL; i++) s.trail[i] = -50.0f + (float)i * 0.6f;
    s.n_trail = DIRUI_TRAIL;
    render("left", &s);
    s.deg = 5.0f; s.laf = 48.0f; s.confidence = 0.4f;
    render("ahead", &s);
    s.active = false;
    render("listening", &s);
    printf(s_fail ? "%d FAILED\n" : "all passed\n", s_fail);
    return s_fail != 0;
}
