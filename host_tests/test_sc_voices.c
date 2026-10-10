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

    /* Clarity: boosts 4 kHz relative to 300 Hz by about the shelf's +6 dB
       less the pre-gain, never wraps a full-scale square wave, and leaves
       samples alone when off. */
    clarity_t c;
    static int16_t s300[3200], s4k[3200];
    for (int i = 0; i < 3200; i++) {
        s300[i] = (int16_t)(10000.0 * sin(2 * 3.14159265 * 300 * i / 16000.0));
        s4k[i] = (int16_t)(10000.0 * sin(2 * 3.14159265 * 4000 * i / 16000.0));
    }
    clarity_init(&c, true);
    clarity_run(&c, s300, 3200);
    clarity_init(&c, true);
    clarity_run(&c, s4k, 3200);
    double sum300 = 0, sum4k = 0;
    for (int i = 200; i < 3200; i++) {                          /* past the filter's brief settling */
        sum300 += (double)s300[i] * s300[i];
        sum4k += (double)s4k[i] * s4k[i];
    }
    double rms300 = sqrt(sum300 / 3000), rms4k = sqrt(sum4k / 3000);
    double ratio = rms4k / rms300;
    CHECK(ratio > 1.7 && ratio < 2.3);

    static int16_t sq[400];
    for (int i = 0; i < 400; i++) sq[i] = (i / 20) % 2 == 0 ? 32767 : -32768;  /* full-scale square wave */
    clarity_init(&c, true);
    int16_t prev_out = 0;
    int prev_in_sign = 0;
    int wraps = 0;
    for (int i = 0; i < 400; i++) {
        int16_t one[1] = { sq[i] };
        clarity_run(&c, one, 1);
        int in_sign = sq[i] > 0 ? 1 : -1;
        if (i > 0 && in_sign == prev_in_sign && one[0] != 0 && prev_out != 0 &&
            ((one[0] > 0) != (prev_out > 0)))
            wraps++;
        prev_out = one[0];
        prev_in_sign = in_sign;
    }
    CHECK(wraps == 0);

    static int16_t keep[800], keep0[800];
    for (int i = 0; i < 800; i++) keep[i] = (int16_t)(20000.0 * sin(2 * 3.14159265 * 220 * i / 16000.0));
    memcpy(keep0, keep, sizeof keep);
    clarity_init(&c, false);
    clarity_run(&c, keep, 800);
    CHECK(memcmp(keep, keep0, sizeof keep) == 0);

    char *body = elevenlabs_body("Hi", 1.0f);
    CHECK(body && strstr(body, "\"model_id\":\"eleven_flash_v2_5\"") && !strstr(body, "voice_settings"));
    free(body);
    body = elevenlabs_body("Hi", 0.85f);
    CHECK(body && strstr(body, "\"voice_settings\":{\"speed\":0.85"));
    free(body);

    printf(fails ? "%d FAILED\n" : "sc_voices: all passed\n", fails);
    return fails != 0;
}
