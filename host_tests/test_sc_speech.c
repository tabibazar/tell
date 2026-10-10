/* stan-claw's speech formats: the WAV it sends Deepgram, Deepgram's reply,
   and the body it sends ElevenLabs. */
#include "speechfmt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static unsigned u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

int main(void)
{
    uint8_t h[44];
    wav_header(h, 16000, 1000);
    CHECK(memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVEfmt ", 8) == 0 && memcmp(h + 36, "data", 4) == 0);
    CHECK(u32(h + 4) == 36 + 2000 && u32(h + 16) == 16 && h[20] == 1 && h[22] == 1);
    CHECK(u32(h + 24) == 16000 && u32(h + 28) == 32000 && h[32] == 2 && h[34] == 16 && u32(h + 40) == 2000);

    char t[200];
    CHECK(deepgram_transcript("{\"results\":{\"channels\":[{\"alternatives\":[{\"transcript\":\"What time is it?\",\"confidence\":0.98}]}]}}", t, sizeof t));
    CHECK(strcmp(t, "What time is it?") == 0);
    CHECK(deepgram_transcript("{\"results\":{\"channels\":[{\"alternatives\":[{\"transcript\":\"\"}]}]}}", t, sizeof t) && t[0] == 0);
    CHECK(!deepgram_transcript("{\"err_code\":\"INVALID_AUTH\"}", t, sizeof t));
    CHECK(!deepgram_transcript("nope", t, sizeof t));

    char *b = elevenlabs_body("Say \"hi\"\n");
    CHECK(b && strstr(b, "\"text\":\"Say \\\"hi\\\"\\n\"") && strstr(b, "\"model_id\":\"eleven_flash_v2_5\""));
    free(b);

    printf(fails ? "%d FAILED\n" : "sc_speech: all passed\n", fails);
    return fails != 0;
}
