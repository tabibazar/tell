#include "stt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "esp_heap_caps.h"
#include "https.h"
#include "speechfmt.h"

int stt_transcribe(const int16_t *pcm, int samples, char *out, size_t n)
{
    out[0] = 0;
    char key[CFG_VAL], auth[CFG_VAL + 8];
    cfg_get(CFG_DEEPGRAM_KEY, key, sizeof key);
    if (!key[0]) return 0;
    snprintf(auth, sizeof auth, "Token %s", key);
    size_t len = 44 + (size_t)samples * 2;
    uint8_t *wav = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!wav) return -1;
    wav_header(wav, 16000, samples);
    memcpy(wav + 44, pcm, (size_t)samples * 2);
    https_hdr_t h[] = { { "Authorization", auth } };
    char *resp = NULL;
    int code = https_post("https://api.deepgram.com/v1/listen?model=nova-3&smart_format=true",
                          h, 1, "audio/wav", wav, len, 15000, &resp, 16384, NULL, NULL);
    free(wav);
    if (code == 200 && !(resp && deepgram_transcript(resp, out, n))) code = -1;
    free(resp);
    return code;
}
