/* stan-claw's end-of-speech detector, on made-up audio: quiet room, then a
   voice, then quiet; pure silence; a cough; talking past the cap. */
#include "vad.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define FS 16000
#define FRAME 320   /* 20 ms */

static double s_din;          /* extra steady noise (a fan, a crowd's hum), RMS-ish amplitude */

static double noise1(unsigned *seed)
{
    *seed = *seed * 1103515245u + 12345u;
    return ((int)(*seed >> 16 & 0x7FFF) - 16384) / 16384.0;
}

static void frame(int16_t *f, double amp, unsigned *seed)
{
    for (int i = 0; i < FRAME; i++) {
        double noise = noise1(seed) * 60.0 + noise1(seed) * s_din * 1.73;   /* ~35 RMS room noise + the din */
        double v = noise + amp * sin(2 * M_PI * 220 * i / FS);
        f[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

/* Feeds `ms` of audio at `amp`; returns the last result. *took: the ms it ran. */
static vad_result_t run_t(vad_t *v, int ms, double amp, unsigned *seed, int *took)
{
    int16_t f[FRAME];
    vad_result_t r = VAD_WAITING;
    for (int t = 0; t < ms; t += 20) {
        frame(f, amp, seed);
        r = vad_feed(v, f, FRAME);
        if (took) *took = t + 20;
        if (r == VAD_DONE || r == VAD_SILENT || r == VAD_CAPPED) return r;
    }
    return r;
}

static vad_result_t run(vad_t *v, int ms, double amp, unsigned *seed) { return run_t(v, ms, amp, seed, NULL); }

/* Speech that comes and goes like words: `amp` for 200 ms, a 60 ms gap, for `ms`. */
static vad_result_t talk(vad_t *v, int ms, double amp, unsigned *seed)
{
    vad_result_t r = VAD_WAITING;
    for (int t = 0; t < ms; t += 260) {
        r = run(v, 200, amp, seed);
        if (r == VAD_DONE || r == VAD_SILENT || r == VAD_CAPPED) return r;
        r = run(v, 60, amp * 0.15, seed);
        if (r == VAD_DONE || r == VAD_SILENT || r == VAD_CAPPED) return r;
    }
    return r;
}

int main(void)
{
    unsigned seed = 1;
    vad_t v;

    vad_init(&v, FS, 15000);
    CHECK(run(&v, 500, 0, &seed) == VAD_WAITING);
    CHECK(run(&v, 1500, 3000, &seed) == VAD_SPEAKING);
    CHECK(run(&v, 1500, 0, &seed) == VAD_DONE);
    CHECK(vad_speech_ms(&v) >= 1400 && vad_speech_ms(&v) <= 1600);

    vad_init(&v, FS, 3000);
    CHECK(run(&v, 4000, 0, &seed) == VAD_SILENT);           /* nobody spoke */

    vad_init(&v, FS, 15000);
    run(&v, 400, 0, &seed);
    run(&v, 100, 3000, &seed);                               /* a cough */
    CHECK(run(&v, 1500, 0, &seed) == VAD_SILENT);

    vad_init(&v, FS, 2000);
    run(&v, 400, 0, &seed);
    CHECK(run(&v, 3000, 3000, &seed) == VAD_CAPPED);         /* still talking at the cap */

    vad_init(&v, FS, 15000);
    run(&v, 400, 0, &seed);
    run(&v, 600, 3000, &seed);
    CHECK(run(&v, 1000, 0, &seed) == VAD_SPEAKING);          /* a whole second's pause is not the end */
    run(&v, 600, 3000, &seed);
    CHECK(run(&v, 1500, 0, &seed) == VAD_DONE);

    /* Noisy rooms. A fan or a TV starts mid-turn: once they stop talking the
       din becomes the room, and the turn still ends (before: never, until the cap). */
    int took;
    vad_init(&v, FS, 25000);
    run(&v, 400, 0, &seed);
    s_din = 700;
    CHECK(talk(&v, 2000, 4000, &seed) == VAD_SPEAKING);
    CHECK(run_t(&v, 9000, 0, &seed, &took) == VAD_DONE);
    CHECK(took <= 5000);                                     /* within the 3 s window + 1.2 s of quiet */
    s_din = 0;

    /* Loud and steady from the start: a voice well above it is heard, and its end is found. */
    vad_init(&v, FS, 25000);
    s_din = 1500;
    CHECK(run(&v, 600, 0, &seed) == VAD_WAITING);
    CHECK(talk(&v, 2500, 8000, &seed) == VAD_SPEAKING);
    CHECK(run_t(&v, 3000, 0, &seed, &took) == VAD_DONE && took <= 1600);
    s_din = 0;

    /* The same din with nobody talking is not speech. */
    vad_init(&v, FS, 5000);
    s_din = 1500;
    CHECK(run(&v, 6000, 0, &seed) == VAD_SILENT);
    s_din = 0;

    /* They start talking the moment it opens (the first 300 ms are voice, not room):
       the room is still found in the next quiet, and the end too. */
    vad_init(&v, FS, 25000);
    CHECK(talk(&v, 1500, 3000, &seed) != VAD_SILENT);
    CHECK(run(&v, 2000, 0, &seed) == VAD_DONE);

    /* A long, unbroken stretch of words (8 s) is not mistaken for the room. */
    vad_init(&v, FS, 25000);
    run(&v, 400, 0, &seed);
    CHECK(talk(&v, 8000, 3000, &seed) == VAD_SPEAKING);
    CHECK(run(&v, 2000, 0, &seed) == VAD_DONE);

    printf(fails ? "%d FAILED\n" : "sc_vad: all passed\n", fails);
    return fails != 0;
}
