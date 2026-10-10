#include "tts.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "config.h"
#include "https.h"
#include "speechfmt.h"

/* Bytes arrive in any size; samples are two bytes: keep an odd one over. */
typedef struct { uint8_t odd; bool have_odd; bool ok; fx_t fx; } play_ctx_t;

static bool play_sink(const uint8_t *d, int len, void *vctx)
{
    play_ctx_t *p = vctx;
    static int16_t s[1100];
    while (len > 0) {
        int k = 0;
        if (p->have_odd) {
            s[k++] = (int16_t)(p->odd | d[0] << 8);
            d++; len--;
            p->have_odd = false;
        }
        while (len >= 2 && k < (int)(sizeof s / sizeof s[0])) {
            s[k++] = (int16_t)(d[0] | d[1] << 8);
            d += 2; len -= 2;
        }
        if (len == 1 && k < (int)(sizeof s / sizeof s[0])) { p->odd = d[0]; p->have_odd = true; len = 0; }
        if (k) fx_run(&p->fx, s, k);                     /* the robots' voices; a no-op for people */
        if (k && !audio_play_chunk(s, k)) { p->ok = false; return false; }
    }
    return true;
}

int tts_speak(const char *text)
{
    char voice[CFG_VAL], fx[16];
    cfg_get(CFG_VOICE, voice, sizeof voice);
    cfg_get(CFG_VOICE_FX, fx, sizeof fx);
    return tts_speak_as(voice, fx_from_name(fx), text);
}

int tts_speak_as(const char *voice, fx_kind_t fxk, const char *text)
{
    char key[CFG_VAL], url[CFG_VAL + 100], sp[16];
    cfg_get(CFG_ELEVEN_KEY, key, sizeof key);
    cfg_get(CFG_SPEED, sp, sizeof sp);
    float speed = sp[0] ? (float)atof(sp) : 1.0f;
    if (speed < 0.7f || speed > 1.2f) speed = 1.0f;
    if (fxk == FX_HAL) speed = speed * 0.9f < 0.7f ? 0.7f : speed * 0.9f;   /* Hal takes his time */
    if (!key[0]) return 0;
    if (!voice[0]) return TTS_NO_VOICE;
    snprintf(url, sizeof url, "https://api.elevenlabs.io/v1/text-to-speech/%s/stream?output_format=pcm_16000", voice);
    char *body = elevenlabs_body(text, speed);
    if (!body) return -1;
    https_hdr_t h[] = { { "xi-api-key", key } };
    static play_ctx_t p;                                 /* the effect's state is big for a stack */
    memset(&p, 0, sizeof p);
    p.ok = true;
    fx_init(&p.fx, fxk);
    audio_play_begin();
    int code = https_post(url, h, 1, "application/json", body, strlen(body), 20000, NULL, 0, play_sink, &p);
    audio_play_end();
    free(body);
    return code == 200 && !p.ok ? -1 : code;
}
