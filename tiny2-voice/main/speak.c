#include "speak.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "https.h"
#include "amp.h"
#include "settings.h"
#include "speechfmt.h"

static const int16_t *volatile s_pcm;
static volatile int s_n;
static volatile bool s_live;

float speak_level(void)
{
    if (!s_live || !amp_playing()) return -1.0f;
    int at = (int)amp_position() - AMP_FS * 3 / 100;       /* past the fade-in */
    if (at < 0 || at >= s_n) return 0.0f;
    int end = at + 320 < s_n ? at + 320 : s_n;               /* the 20 ms playing now */
    double sq = 0;
    for (int i = at; i < end; i++) sq += (double)s_pcm[i] * s_pcm[i];
    float rms = (float)sqrt(sq / (end - at > 0 ? end - at : 1));
    float v = rms / 14000.0f;
    return v > 1.0f ? 1.0f : v;
}

/* The whole sentence is collected first and then played in one go: speech
   streamed straight to the pin crackled wherever the network paused. */
#define MAX_S 30
typedef struct { int16_t *buf; int n, max; uint8_t odd; bool have_odd; } ctx_t;

static bool sink(const uint8_t *d, int len, void *vctx)
{
    ctx_t *c = vctx;
    for (int i = 0; i < len; i++) {
        if (!c->have_odd) { c->odd = d[i]; c->have_odd = true; continue; }
        c->have_odd = false;
        if (c->n == c->max) return false;
        c->buf[c->n++] = (int16_t)(c->odd | d[i] << 8);
    }
    return true;
}

int speak(const char *text)
{
    char key[SET_VAL], voice[SET_VAL], sp[16], url[SET_VAL + 100];
    set_get(SET_ELEVEN_KEY, key, sizeof key);
    set_get(SET_VOICE, voice, sizeof voice);
    set_get(SET_SPEED, sp, sizeof sp);
    if (!key[0]) return 0;
    if (!voice[0]) return -2;
    float speed = sp[0] ? (float)atof(sp) : 1.0f;
    if (speed < 0.7f || speed > 1.2f) speed = 1.0f;
    snprintf(url, sizeof url, "https://api.elevenlabs.io/v1/text-to-speech/%s/stream?output_format=pcm_16000", voice);
    char *body = elevenlabs_body(text, speed);
    if (!body) return -1;
    https_hdr_t h[] = { { "xi-api-key", key } };
    static int16_t *pcm;
    if (!pcm) pcm = heap_caps_malloc(AMP_FS * MAX_S * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm) { free(body); return -1; }
    ctx_t c = { .buf = pcm, .max = AMP_FS * MAX_S };
    int64_t t0 = esp_timer_get_time();
    int code = https_post(url, h, 1, "application/json", body, strlen(body), 20000, NULL, 0, sink, &c);
    int64_t t1 = esp_timer_get_time();
    free(body);
    if (code != 200 || c.n == 0) return code == 200 ? -1 : code;
    int peak = 0;
    double sq = 0;
    for (int i = 0; i < c.n; i++) {
        int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v > peak) peak = v;
        sq += (double)pcm[i] * pcm[i];
    }
    int rms = (int)sqrt(sq / c.n);
    ESP_LOGI("speak", "%.1f s of speech in %.1f s; peak %d, rms %d", c.n / (float)AMP_FS, (t1 - t0) / 1e6, peak, rms);
    /* As loud as a small speaker allows: bring the average up to a third of full
       scale, and round off the peaks that would then clip (tanh) instead of
       chopping them. Speech arrives some 20 dB below full scale. */
    float g = rms > 0 ? 10000.0f / rms : 1.0f;
    if (g > 8.0f) g = 8.0f;
    for (int i = 0; i < c.n; i++) pcm[i] = (int16_t)(31000.0f * tanhf(pcm[i] * g / 31000.0f));
    s_pcm = pcm;
    s_n = c.n;
    amp_begin();
    s_live = true;
    amp_write(pcm, c.n);
    amp_end();
    s_live = false;
    return code;
}

void speak_tone(void)
{
    static int16_t t[AMP_FS * 3 / 5];
    const int n = AMP_FS * 3 / 5, half = n / 2;
    for (int i = 0; i < n; i++) {
        int k = i < half ? i : i - half;
        float env = (k < 300 ? k / 300.0f : 1.0f) * expf(-3.5f * k / half);
        t[i] = (int16_t)(16000 * env * sinf(2 * 3.14159f * (i < half ? 659.0f : 880.0f) * k / AMP_FS));
    }
    amp_begin();
    amp_write(t, n);
    amp_end();
}
