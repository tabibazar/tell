/* liveui: the octave bands hold a sine to its band and level, and the page
   renders (host_tests/renders/liveui/, as PPM, converted with sips or PIL). */
#include "liveui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int s_fail;
static void expect(const char *what, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) s_fail++;
}

static int16_t s_x[LIVEUI_FFT];

static void sine(float hz, float dbfs, int add)
{
    float a = 32767.0f * powf(10.0f, dbfs / 20.0f) * 1.41421356f;
    for (int i = 0; i < LIVEUI_FFT; i++) {
        float v = a * sinf(2.0f * 3.14159265f * hz * (float)i / 16000.0f) + (add ? (float)s_x[i] : 0.0f);
        s_x[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

static uint16_t s_fb[LIVEUI_WIDTH * LIVEUI_HEIGHT];

static void render(const char *name, const liveui_t *s)
{
    canvas_t c;
    canvas_init(&c, s_fb, LIVEUI_WIDTH, LIVEUI_HEIGHT, 1);
    liveui_draw(&c, s);
    char path[128];
    mkdir("renders", 0755);
    mkdir("renders/liveui", 0755);
    snprintf(path, sizeof path, "renders/liveui/%s.ppm", name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6 %d %d 255\n", LIVEUI_WIDTH, LIVEUI_HEIGHT);
    for (int i = 0; i < LIVEUI_WIDTH * LIVEUI_HEIGHT; i++) {
        uint16_t p = s_fb[i];
        unsigned char rgb[3] = { (unsigned char)((p >> 11) * 255 / 31), (unsigned char)(((p >> 5) & 63) * 255 / 63),
                                 (unsigned char)((p & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(void)
{
    float b[LIVEUI_BANDS];

    /* A 1 kHz sine at -20 dBFS: its band reads -20 (+ offset), the others far below. */
    sine(1000.0f, -20.0f, 0);
    liveui_bands(s_x, 16000.0f, 0.0f, b);
    printf("     1 kHz -20 dBFS: %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f\n", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    expect("a 1 kHz sine lands in the 1k band at its level", fabsf(b[4] - (-20.0f)) < 1.0f);
    expect("and the bands two away are 30 dB under", b[2] < -50.0f && b[6] < -50.0f);
    liveui_bands(s_x, 16000.0f, 114.0f, b);
    expect("the offset is added", fabsf(b[4] - 94.0f) < 1.0f);

    /* 63 Hz and 8 kHz, the ends. */
    sine(63.0f, -30.0f, 0);
    liveui_bands(s_x, 16000.0f, 0.0f, b);
    expect("63 Hz lands in the lowest band", fabsf(b[0] + 30.0f) < 2.0f);
    sine(7000.0f, -30.0f, 0);
    liveui_bands(s_x, 16000.0f, 0.0f, b);
    expect("7 kHz lands in the 8k band", fabsf(b[7] + 30.0f) < 1.5f);

    /* Renders: a voice-like mix, loud, quiet, no signal. */
    static liveui_t s;
    memset(&s, 0, sizeof s);
    s.have_signal = true;
    s.laf = 58.0f;
    sine(180.0f, -36.0f, 0); sine(520.0f, -40.0f, 1); sine(1400.0f, -48.0f, 1); sine(3100.0f, -58.0f, 1);
    memcpy(s.wave, s_x, sizeof s.wave);
    liveui_bands(s_x, 16000.0f, 100.0f, s.band);
    for (int i = 0; i < LIVEUI_BANDS; i++) s.peak[i] = s.band[i] + 4.0f;
    render("voice", &s);

    s.laf = 82.0f;
    sine(250.0f, -12.0f, 0); sine(2000.0f, -18.0f, 1);
    memcpy(s.wave, s_x, sizeof s.wave);
    liveui_bands(s_x, 16000.0f, 100.0f, s.band);
    for (int i = 0; i < LIVEUI_BANDS; i++) s.peak[i] = s.band[i] + 2.0f;
    s.calibrated = true;
    render("loud", &s);

    s.laf = 34.0f;
    s.calibrated = false;
    for (int i = 0; i < LIVEUI_FFT; i++) s_x[i] = (int16_t)((i * 7919 % 61) - 30);
    memcpy(s.wave, s_x, sizeof s.wave);
    liveui_bands(s_x, 16000.0f, 100.0f, s.band);
    for (int i = 0; i < LIVEUI_BANDS; i++) s.peak[i] = s.band[i] + 1.0f;
    render("quiet", &s);

    s.have_signal = false;
    render("no_signal", &s);

    printf(s_fail ? "%d FAILED\n" : "all passed\n", s_fail);
    return s_fail != 0;
}
