#include "phrases.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "memory.h"
#include "tts.h"

static const char *TAG = "phrases";

static const char *const TEXT[PH_N] = {
    [PH_MOMENT1] = "One moment, let me check that for you.",
    [PH_MOMENT2] = "Just a moment, I'm looking that up.",
    [PH_MOMENT3] = "Bear with me a second, I'm checking.",
    [PH_OFFLINE] = "I can't reach the internet right now. Your WiFi may be down. I'll keep trying, so please try "
                   "again in a few minutes.",
    [PH_TROUBLE] = "I'm having trouble reaching my helpers right now. Please try again in a minute.",
    [PH_MISSED]  = "Sorry, I didn't hear anything. Just tap the screen when you're ready to talk.",
};

#define MAX_S   8                                /* seconds a phrase may run */
#define MAX_N   (16000 * MAX_S)

static int16_t *s_pcm[PH_N];                    /* PSRAM: each phrase as recorded, before the voice's effect */
static int s_n[PH_N];
static char s_key[CFG_VAL + 24];                /* the voice and speed they were recorded in */

const char *phrase_text(phrase_t p) { return TEXT[p]; }

static void want_key(char *out, size_t n)
{
    char voice[CFG_VAL], sp[16];
    cfg_get(CFG_VOICE, voice, sizeof voice);
    cfg_get(CFG_SPEED, sp, sizeof sp);
    snprintf(out, n, "%s@%s", voice, sp[0] ? sp : "1.0");
}

void phrases_load(void)
{
    char *k = memory_read("phkey.txt", sizeof s_key - 1);
    snprintf(s_key, sizeof s_key, "%s", k ? k : "");
    free(k);
    for (int i = 0; i < PH_N; i++) {
        char name[16];
        snprintf(name, sizeof name, "ph%d.pcm", i);
        unsigned char *b = NULL;
        int len = memory_read_bin(name, &b, MAX_N * 2);
        free(s_pcm[i]);
        s_pcm[i] = (int16_t *)b;
        s_n[i] = len > 0 ? len / 2 : 0;
    }
}

bool phrases_stale(void)
{
    char want[sizeof s_key];
    want_key(want, sizeof want);
    if (strcmp(want, s_key) != 0) return true;
    for (int i = 0; i < PH_N; i++) if (!s_n[i]) return true;
    return false;
}

bool phrases_refresh_one(void)
{
    char want[sizeof s_key];
    want_key(want, sizeof want);
    if (strcmp(want, s_key) != 0) {                /* a new voice or speed: record them all again */
        for (int i = 0; i < PH_N; i++) s_n[i] = 0;
        snprintf(s_key, sizeof s_key, "%s", want);
        memory_write("phkey.txt", s_key);
    }
    for (int i = 0; i < PH_N; i++) {
        if (s_n[i]) continue;
        if (!s_pcm[i]) s_pcm[i] = heap_caps_malloc(MAX_N * 2, MALLOC_CAP_SPIRAM);
        if (!s_pcm[i]) return false;
        int n = 0;
        int code = tts_capture(TEXT[i], s_pcm[i], MAX_N, &n);
        if (code != 200 || n < 1600) { ESP_LOGW(TAG, "phrase %d: %d", i, code); return false; }
        char name[16];
        snprintf(name, sizeof name, "ph%d.pcm", i);
        memory_write_bin(name, s_pcm[i], (size_t)n * 2);
        s_n[i] = n;
        ESP_LOGI(TAG, "phrase %d recorded: %.1f s", i, n / 16000.0f);
        return true;
    }
    return true;
}

bool phrase_play(phrase_t p)
{
    if (p < 0 || p >= PH_N || !s_n[p]) return false;
    tts_play_pcm(s_pcm[p], s_n[p]);
    return true;
}
