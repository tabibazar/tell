#include "speechfmt.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

static void le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void le16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

void wav_header(uint8_t h[44], int fs, int samples)
{
    uint32_t data = (uint32_t)samples * 2;
    memcpy(h, "RIFF", 4);
    le32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16);
    le16(h + 20, 1);              /* PCM */
    le16(h + 22, 1);              /* mono */
    le32(h + 24, (uint32_t)fs);
    le32(h + 28, (uint32_t)fs * 2);
    le16(h + 32, 2);
    le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    le32(h + 40, data);
}

bool deepgram_transcript(const char *json, char *out, size_t n)
{
    cJSON *j = cJSON_Parse(json);
    if (!j) return false;
    const cJSON *ch = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(j, "results"), "channels"), 0);
    const cJSON *alt = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(ch, "alternatives"), 0);
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(alt, "transcript");
    bool ok = cJSON_IsString(t);
    if (ok) snprintf(out, n, "%s", t->valuestring);
    cJSON_Delete(j);
    return ok;
}

char *elevenlabs_body(const char *text, float speed)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "text", text);
    cJSON_AddStringToObject(o, "model_id", "eleven_flash_v2_5");
    if (speed < 0.99f || speed > 1.01f) {
        cJSON *vs = cJSON_AddObjectToObject(o, "voice_settings");
        cJSON_AddNumberToObject(vs, "speed", speed);
    }
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}
