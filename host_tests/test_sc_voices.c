/* stan-claw's voice list: an ElevenLabs /v1/voices reply cut down for the
   settings page, and the speed in the speech request. */
#include "speechfmt.h"
#include "voices.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    const char *reply =
        "{\"voices\":["
        "{\"voice_id\":\"EXAVITQu4vr4xnSDxMaL\",\"name\":\"Bella - Professional, Bright, Warm\",\"category\":\"premade\","
        " \"labels\":{\"gender\":\"female\",\"accent\":\"american\",\"age\":\"middle_aged\"},\"samples\":null},"
        "{\"voice_id\":\"abc123\",\"name\":\"Peppa\",\"labels\":{}},"
        "{\"voice_id\":42,\"name\":\"broken\"},"
        "{\"voice_id\":\"x9\",\"name\":\"Ren\\u00e9e - Calm\",\"labels\":{\"accent\":\"french\"}}"
        "],\"has_more\":false}";
    voice_t v[VOICES_MAX];
    int n = voices_parse(reply, v, VOICES_MAX);
    CHECK(n == 3);                                            /* the broken one is skipped */
    CHECK(strcmp(v[0].id, "EXAVITQu4vr4xnSDxMaL") == 0 && strcmp(v[0].name, "Bella") == 0);
    CHECK(strcmp(v[0].desc, "Professional, Bright, Warm") == 0);
    CHECK(strcmp(v[0].tags, "female, american, middle aged") == 0);
    CHECK(strcmp(v[1].name, "Peppa") == 0 && v[1].desc[0] == 0 && v[1].tags[0] == 0);
    CHECK(strcmp(v[2].name, "Rene") == 0 && strcmp(v[2].tags, "french") == 0);   /* ASCII only */
    CHECK(voices_parse(reply, v, 1) == 1);
    CHECK(voices_parse("{\"detail\":{\"status\":\"invalid_api_key\"}}", v, VOICES_MAX) == -1);
    CHECK(voices_parse("nope", v, VOICES_MAX) == -1);

    char *b = elevenlabs_body("Hi", 1.0f);
    CHECK(b && strstr(b, "\"model_id\":\"eleven_flash_v2_5\"") && !strstr(b, "voice_settings"));   /* normal: no settings */
    free(b);
    b = elevenlabs_body("Hi", 0.85f);
    CHECK(b && strstr(b, "\"voice_settings\":{\"speed\":0.85"));
    free(b);

    printf(fails ? "%d FAILED\n" : "sc_voices: all passed\n", fails);
    return fails != 0;
}
