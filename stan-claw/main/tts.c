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
typedef struct { uint8_t odd; bool have_odd; bool ok; fx_t fx; clarity_t clar; } play_ctx_t;

static volatile bool s_stop;
static volatile int s_played;          /* samples played of the speech in progress, for the captions */
void tts_abort(void) { s_stop = true; }
int tts_played(void) { return s_played; }

static bool play_sink(const uint8_t *d, int len, void *vctx)
{
    play_ctx_t *p = vctx;
    if (s_stop) return false;                           /* abandon the rest of the stream */
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
        if (k) clarity_run(&p->clar, s, k);              /* the consonants lifted, for ears that miss them */
        if (k && !audio_play_chunk(s, k)) { p->ok = false; return false; }
        s_played += k;
    }
    return true;
}

static int speak(const char *voice, fx_kind_t fxk, const char *text, float speed);

/* Collecting speech instead of playing it: a recorded phrase for later. */
typedef struct { int16_t *buf; int max, n; uint8_t odd; bool have_odd; } cap_ctx_t;

static bool cap_sink(const uint8_t *d, int len, void *vctx)
{
    cap_ctx_t *c = vctx;
    for (int i = 0; i < len; i++) {
        if (!c->have_odd) { c->odd = d[i]; c->have_odd = true; continue; }
        c->have_odd = false;
        if (c->n == c->max) return false;
        c->buf[c->n++] = (int16_t)(c->odd | d[i] << 8);
    }
    return true;
}

int tts_capture(const char *text, int16_t *out, int max, int *n)
{
    char key[CFG_VAL], voice[CFG_VAL], url[CFG_VAL + 100];
    cfg_get(CFG_ELEVEN_KEY, key, sizeof key);
    cfg_get(CFG_VOICE, voice, sizeof voice);
    *n = 0;
    if (!key[0]) return 0;
    if (!voice[0]) return TTS_NO_VOICE;
    snprintf(url, sizeof url, "https://api.elevenlabs.io/v1/text-to-speech/%s/stream?output_format=pcm_16000", voice);
    char *body = elevenlabs_body(text, tts_speed());
    if (!body) return -1;
    https_hdr_t h[] = { { "xi-api-key", key } };
    cap_ctx_t c = { .buf = out, .max = max };
    int code = https_post(url, h, 1, "application/json", body, strlen(body), 20000, NULL, 0, cap_sink, &c);
    free(body);
    *n = c.n;
    return code;
}

void tts_play_pcm(const int16_t *pcm, int n)
{
    static play_ctx_t p;
    char fx[16], cl[4];
    cfg_get(CFG_VOICE_FX, fx, sizeof fx);
    cfg_get(CFG_CLARITY, cl, sizeof cl);
    memset(&p, 0, sizeof p);
    p.ok = true;
    fx_init(&p.fx, fx_from_name(fx));
    clarity_init(&p.clar, cl[0] != '0');
    s_stop = false;
    s_played = 0;
    audio_play_begin();
    play_sink((const uint8_t *)pcm, n * 2, &p);          /* little-endian, as ElevenLabs sends it */
    audio_play_end();
}

int tts_speak(const char *text) { return tts_speak_slow(text, 0); }

int tts_speak_slow(const char *text, float speed)
{
    char voice[CFG_VAL], fx[16];
    cfg_get(CFG_VOICE, voice, sizeof voice);
    cfg_get(CFG_VOICE_FX, fx, sizeof fx);
    return speak(voice, fx_from_name(fx), text, speed);
}

int tts_speak_as(const char *voice, fx_kind_t fxk, const char *text) { return speak(voice, fxk, text, 0); }

float tts_speed(void)
{
    char sp[16];
    cfg_get(CFG_SPEED, sp, sizeof sp);
    float speed = sp[0] ? (float)atof(sp) : 1.0f;
    return speed < 0.7f || speed > 1.2f ? 1.0f : speed;
}

static int speak(const char *voice, fx_kind_t fxk, const char *text, float speed)
{
    char key[CFG_VAL], url[CFG_VAL + 100], cl[4];
    cfg_get(CFG_ELEVEN_KEY, key, sizeof key);
    cfg_get(CFG_CLARITY, cl, sizeof cl);
    if (speed < 0.7f || speed > 1.2f) speed = tts_speed();
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
    clarity_init(&p.clar, cl[0] != '0');                 /* on unless switched off */
    s_stop = false;
    s_played = 0;
    audio_play_begin();
    int code = https_post(url, h, 1, "application/json", body, strlen(body), 20000, NULL, 0, play_sink, &p);
    audio_play_end();
    free(body);
    return code == 200 && !p.ok ? -1 : code;
}
