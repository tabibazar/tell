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

static void frame(int16_t *f, double amp, unsigned *seed)
{
    for (int i = 0; i < FRAME; i++) {
        *seed = *seed * 1103515245u + 12345u;
        double noise = ((int)(*seed >> 16 & 0x7FFF) - 16384) / 16384.0 * 60.0;   /* ~35 RMS room noise */
        f[i] = (int16_t)(noise + amp * sin(2 * M_PI * 220 * i / FS));
    }
}

/* Feeds `ms` of audio at `amp`; returns the last result. */
static vad_result_t run(vad_t *v, int ms, double amp, unsigned *seed)
{
    int16_t f[FRAME];
    vad_result_t r = VAD_WAITING;
    for (int t = 0; t < ms; t += 20) {
        frame(f, amp, seed);
        r = vad_feed(v, f, FRAME);
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
    CHECK(run(&v, 1000, 0, &seed) == VAD_DONE);
    CHECK(vad_speech_ms(&v) >= 1400 && vad_speech_ms(&v) <= 1600);

    vad_init(&v, FS, 3000);
    CHECK(run(&v, 4000, 0, &seed) == VAD_SILENT);           /* nobody spoke */

    vad_init(&v, FS, 15000);
    run(&v, 400, 0, &seed);
    run(&v, 100, 3000, &seed);                               /* a cough */
    CHECK(run(&v, 1000, 0, &seed) == VAD_SILENT);

    vad_init(&v, FS, 2000);
    run(&v, 400, 0, &seed);
    CHECK(run(&v, 3000, 3000, &seed) == VAD_CAPPED);         /* still talking at the cap */

    vad_init(&v, FS, 15000);
    run(&v, 400, 0, &seed);
    run(&v, 600, 3000, &seed);
    CHECK(run(&v, 500, 0, &seed) == VAD_SPEAKING);           /* a 500 ms pause is not the end */
    run(&v, 600, 3000, &seed);
    CHECK(run(&v, 1000, 0, &seed) == VAD_DONE);

    printf(fails ? "%d FAILED\n" : "sc_vad: all passed\n", fails);
    return fails != 0;
}
