/* stan-claw's ten voices (four men, four women, two machines), the robots'
   effect, and the speed in the speech request. */
#include "fx.h"
#include "speechfmt.h"
#include "voices.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    int men = 0, women = 0, robots = 0;
    for (int i = 0; i < VOICES_N; i++) {
        CHECK(strlen(VOICES[i].id) == 20 && VOICES[i].name[0]);
        if (VOICES[i].fx != FX_NONE) robots++;
        else if (strncmp(VOICES[i].tags, "male", 4) == 0) men++;
        else if (strncmp(VOICES[i].tags, "female", 6) == 0) women++;
    }
    CHECK(men == 4 && women == 4 && robots == 2);
    CHECK(voices_find("CwhRBWXzGAHq8TQ4Fs17", "") == 0);          /* Roger */
    CHECK(voices_find("cjVigY5qzO86Huf0OWal", "hal") == 8);       /* Hal: Eric's voice through the effect */
    CHECK(voices_find("cjVigY5qzO86Huf0OWal", "") == -1);         /* plain Eric is not on the list */
    CHECK(voices_find("nope", "") == -1 && voices_find(NULL, NULL) == -1);
    CHECK(fx_from_name("jarvis") == FX_JARVIS && strcmp(fx_name(FX_HAL), "hal") == 0);

    /* The effect: none leaves it alone; the robots change it, never clip,
       and keep silence silent. */
    static int16_t a[1600], b[1600];
    for (int i = 0; i < 1600; i++) a[i] = (int16_t)(30000 * sin(2 * 3.14159265 * 220 * i / 16000.0));
    fx_t f;
    for (int k = FX_NONE; k <= FX_JARVIS; k++) {
        memcpy(b, a, sizeof a);
        fx_init(&f, (fx_kind_t)k);
        fx_run(&f, b, 800);
        fx_run(&f, b + 800, 800);                                  /* in chunks, as it streams */
        int diff = 0, peak = 0;
        for (int i = 0; i < 1600; i++) { diff += b[i] != a[i]; peak = abs(b[i]) > peak ? abs(b[i]) : peak; }
        if (k == FX_NONE) CHECK(diff == 0);
        else CHECK(diff > 1000 && peak <= 32767 && peak > 8000);
    }
    memset(b, 0, sizeof b);
    fx_init(&f, FX_JARVIS);
    fx_run(&f, b, 1600);
    int loud = 0;
    for (int i = 0; i < 1600; i++) loud |= b[i];
    CHECK(loud == 0);

    char *body = elevenlabs_body("Hi", 1.0f);
    CHECK(body && strstr(body, "\"model_id\":\"eleven_flash_v2_5\"") && !strstr(body, "voice_settings"));
    free(body);
    body = elevenlabs_body("Hi", 0.85f);
    CHECK(body && strstr(body, "\"voice_settings\":{\"speed\":0.85"));
    free(body);

    printf(fails ? "%d FAILED\n" : "sc_voices: all passed\n", fails);
    return fails != 0;
}
