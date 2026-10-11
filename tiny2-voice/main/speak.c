#include "speak.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "https.h"
#include "pdm.h"
#include "settings.h"
#include "speechfmt.h"

typedef struct { uint8_t odd; bool have_odd; } ctx_t;

static bool sink(const uint8_t *d, int len, void *vctx)
{
    ctx_t *c = vctx;
    static int16_t s[512];
    int k = 0;
    for (int i = 0; i < len; i++) {
        if (!c->have_odd) { c->odd = d[i]; c->have_odd = true; continue; }
        c->have_odd = false;
        s[k++] = (int16_t)(c->odd | d[i] << 8);
        if (k == 512) { if (!pdm_write(s, k)) return false; k = 0; }
    }
    return k == 0 || pdm_write(s, k);
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
    ctx_t c = { 0 };
    pdm_begin();
    int code = https_post(url, h, 1, "application/json", body, strlen(body), 20000, NULL, 0, sink, &c);
    pdm_end();
    free(body);
    return code;
}

void speak_tone(void)
{
    static int16_t t[PDM_FS * 3 / 5];
    const int n = PDM_FS * 3 / 5, half = n / 2;
    for (int i = 0; i < n; i++) {
        int k = i < half ? i : i - half;
        float env = (k < 300 ? k / 300.0f : 1.0f) * expf(-3.5f * k / half);
        t[i] = (int16_t)(16000 * env * sinf(2 * 3.14159f * (i < half ? 659.0f : 880.0f) * k / PDM_FS));
    }
    pdm_begin();
    pdm_write(t, n);
    pdm_end();
}
