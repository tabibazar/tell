#include "drift.h"
#include "palette.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

#define W 320
#define H 172

int main(void)
{
    drift_t d;
    drift_init(&d, 15);

    float lo, hi, ppm;
    expect("an empty log has no range", !drift_range(&d, true, &lo, &hi));
    expect("and nothing in it", drift_count(&d) == 0);
    expect("and no slope to report", !drift_ppm(&d, &ppm));

    drift_add(&d, 0.0f, true, 24.0f);
    drift_add(&d, 5.0f, true, 24.25f);
    drift_add(&d, 10.0f, true, 24.5f);
    expect("three samples", drift_count(&d) == 3);
    expect("oldest first", drift_phase(&d, 0) == 0.0f);
    expect("newest last", drift_phase(&d, 2) == 10.0f);
    expect("the span is the samples times the interval", drift_span_s(&d) == 30);
    expect("three points over half a minute prove nothing", !drift_ppm(&d, &ppm));

    /*
     * The assertion this module exists for. A board running fast by a known
     * amount must read back as that many ppm: 20 ppm is 20 microseconds per
     * second, so at one sample every 15 seconds the phase gains 0.3 ms a
     * sample. If the arithmetic or the units are wrong anywhere, this is
     * where it shows, and no amount of looking at the chart would reveal it.
     */
    drift_init(&d, 15);
    for (int i = 0; i < 120; i++) drift_add(&d, (float)i * 0.3f, true, 24.0f);
    expect("a known slope reads back as its ppm",
           drift_ppm(&d, &ppm) && fabsf(ppm - 20.0f) < 0.01f);

    /* A board running slow reads negative, which is the whole point of
       keeping the sign rather than an absolute error. */
    drift_init(&d, 15);
    for (int i = 0; i < 120; i++) drift_add(&d, (float)i * -0.15f, true, 24.0f);
    expect("a slow board reads negative",
           drift_ppm(&d, &ppm) && fabsf(ppm + 10.0f) < 0.01f);

    /* Noise of the size the poll actually produces must not move the answer
       much: that is why this is a fit and not a first-to-last difference. */
    drift_init(&d, 15);
    for (int i = 0; i < 160; i++) {
        float jitter = (float)((i * 37) % 33) - 16.0f;     /* +/-16 ms, no libc rand */
        drift_add(&d, (float)i * 0.3f + jitter, true, 24.0f);
    }
    expect("poll noise does not swamp the fit",
           drift_ppm(&d, &ppm) && fabsf(ppm - 20.0f) < 2.0f);

    /* A perfectly steady board is zero ppm, not "no reading". */
    drift_init(&d, 15);
    for (int i = 0; i < 40; i++) drift_add(&d, 7.0f, true, 24.0f);
    expect("a steady board is zero ppm",
           drift_ppm(&d, &ppm) && fabsf(ppm) < 0.001f);

    /* The ring: once full the oldest falls off the left. */
    drift_init(&d, 15);
    for (int i = 0; i < DRIFT_MAX + 25; i++) drift_add(&d, (float)i, true, 20.0f);
    expect("it fills and stops growing", drift_count(&d) == DRIFT_MAX);
    expect("the oldest has rolled off", drift_phase(&d, 0) == 25.0f);
    expect("and the newest is the last one in",
           drift_phase(&d, DRIFT_MAX - 1) == (float)(DRIFT_MAX + 24));

    /* A chip that will not report its temperature still charts the phase. */
    drift_init(&d, 15);
    drift_add(&d, 3.0f, false, 0.0f);
    float x;
    expect("a missing temperature reads as missing", !drift_xtal(&d, 0, &x));
    expect("but the phase is still there", drift_phase(&d, 0) == 3.0f);
    expect("there is still a phase range", drift_range(&d, true, &lo, &hi));
    expect("but no temperature range at all", !drift_range(&d, false, &lo, &hi));

    /* A flat trace must not divide by zero when scaled. The phase range is
       of the change, so a board that has not moved is centred on zero however
       far its raw phase happens to sit from midnight. */
    drift_init(&d, 15);
    for (int i = 0; i < 20; i++) drift_add(&d, 4.0f, true, 24.0f);
    expect("a flat log still has a range", drift_range(&d, true, &lo, &hi));
    expect("which is not zero wide", hi > lo);
    expect("and is centred on no movement", lo < 0.0f && hi > 0.0f);

    /*
     * The slip is what the page shows, and it must not inherit the arbitrary
     * offset a board picks up when it sets its clock from the chip to the
     * nearest second. Two boards that gained ten milliseconds report ten,
     * whether they started at -377 or +412.
     */
    drift_init(&d, 15);
    drift_add(&d, -377.0f, true, 24.0f);
    drift_add(&d, -367.0f, true, 24.0f);
    expect("the slip is the movement, not the offset",
           drift_slip_ms(&d) == 10.0f && drift_phase(&d, 1) == -367.0f);
    drift_init(&d, 15);
    drift_add(&d, 412.0f, true, 24.0f);
    drift_add(&d, 422.0f, true, 24.0f);
    expect("and it is the same movement from a different offset",
           drift_slip_ms(&d) == 10.0f);
    expect("the record starts at no movement", drift_rel(&d, 0) == 0.0f);

    /*
     * Every pixel inside the framebuffer, for an empty log, a full one, a
     * flat one and wildly out-of-range values. A chart scales user data into
     * pixels, and one bad sample is exactly how that ends up off the end of
     * the buffer.
     */
    {
        static uint16_t guarded[8 + W * H + 8];
        canvas_t c;
        int intact = 1;
        float cases[5] = { 0.0f, 1.0f, 1000.0f, -1e6f, 1e9f };
        for (int k = 0; k < 5; k++) {
            memset(guarded, 0xAB, sizeof guarded);
            canvas_init(&c, guarded + 8, W, H, 1);
            drift_init(&d, 15);
            for (int i = 0; i < (k == 0 ? 0 : DRIFT_MAX + 5); i++)
                drift_add(&d, cases[k] + (float)(i % 7), true, cases[k] / 10.0f);
            drift_draw(&d, &c, 3);
            for (int i = 0; i < 8; i++)
                if (guarded[i] != 0xABAB || guarded[8 + W * H + i] != 0xABAB) intact = 0;
        }
        expect("the chart never draws outside the framebuffer", intact);

        /*
         * It draws both traces, and nothing above the rows it was given.
         * The samples carry poll noise here, as real ones do: without it the
         * fit lies exactly on top of every point and there is no scatter to
         * find, which would make this assertion pass for the wrong reason.
         */
        memset(guarded, 0, sizeof guarded);
        canvas_init(&c, guarded + 8, W, H, 1);
        drift_init(&d, 15);
        for (int i = 0; i < 100; i++) {
            float jitter = (float)((i * 37) % 33) - 16.0f;
            drift_add(&d, (float)i * 0.3f + jitter, true,
                      24.0f + (float)(i % 5) * 0.25f);
        }
        drift_draw(&d, &c, 3);

        int fit = 0, dots = 0, temp = 0;
        for (int i = 0; i < W * H; i++) {
            if (c.fb[i] == PAL_A0) fit++;
            if (c.fb[i] == pal_darken(PAL_A0)) dots++;
            if (c.fb[i] == pal_darken(PAL_A1)) temp++;
        }
        expect("the samples are drawn as a scatter", dots > 80);
        expect("the fit is drawn over them", fit > 80);
        expect("the temperature is drawn beside them", temp > 80);

        /*
         * The fit the chart draws and the ppm the page prints must come from
         * the same arithmetic. A trend line computed separately from its
         * headline number is a chart that will eventually disagree with
         * itself, and nobody looking at it would be able to tell which half
         * was wrong.
         */
        {
            float slope = 0.0f, ppm = 0.0f;
            expect("the fit and the figure agree",
                   drift_fit(&d, &slope, NULL) && drift_ppm(&d, &ppm)
                   && fabsf(slope * 1000.0f - ppm) < 0.001f);
        }

        int above = 0;
        for (int y = 0; y < 3 * 24; y++)
            for (int xx = 0; xx < W; xx++)
                if (c.fb[y * W + xx] != PAL_BG) above++;
        expect("the rows above the chart are left to the caller", above == 0);

        /* The start of the record is marked. Because the chart works in the
           change rather than the raw phase, that line is always on the panel
           and always means the same thing -- above it the board has gained on
           the chip, below it the board has lost. */
        int zero_dots = 0;
        for (int i = 0; i < W * H; i++) if (c.fb[i] == PAL_DIM) zero_dots++;
        expect("the start of the record is marked", zero_dots > 20);

        /* And not marked when the whole trace has left it behind. */
        /* And still marked when the raw phase sits far from midnight's zero,
           which is the case on every board that set its clock from the chip. */
        memset(guarded, 0, sizeof guarded);
        canvas_init(&c, guarded + 8, W, H, 1);
        drift_init(&d, 15);
        for (int i = 0; i < 100; i++) drift_add(&d, -377.0f + (float)i * 0.3f, true, 24.0f);
        drift_draw(&d, &c, 3);
        zero_dots = 0;
        for (int i = 0; i < W * H; i++) if (c.fb[i] == PAL_DIM) zero_dots++;
        expect("and marked whatever the raw offset is", zero_dots > 20);

        /*
         * A few noisy samples must draw the scatter and NOT the trend: a line
         * fitted to five jittery points is a line through the noise, and
         * drawing it beside a figure that says "settling" would have the
         * chart contradicting its own caption.
         */
        memset(guarded, 0, sizeof guarded);
        canvas_init(&c, guarded + 8, W, H, 1);
        drift_init(&d, 15);
        for (int i = 0; i < 5; i++)
            drift_add(&d, -377.0f + (float)((i * 17) % 29), true, 24.0f);
        drift_draw(&d, &c, 3);
        /* With no line over them the samples carry the full colour: they are
           the whole chart at this point, and dimmed they read as an empty
           page rather than a young one. */
        int early_dim = 0, early_bright = 0;
        for (int i = 0; i < W * H; i++) {
            if (c.fb[i] == PAL_A0) early_bright++;
            if (c.fb[i] == pal_darken(PAL_A0)) early_dim++;
        }
        expect("a young record draws its samples at full strength", early_bright > 0);
        expect("and dims none of them", early_dim == 0);
        /* No trend: a line fitted to five jittery points is a line through
           the noise. It must be absent, not merely faint -- so the only
           bright pixels are the dots, which are 2x2 each. */
        expect("but no trend line through them", early_bright <= 5 * 4);

        /*
         * And those few samples spread across the panel rather than huddling
         * in the left corner. One sample per two pixels would put five of
         * them in ten pixels of three hundred and twenty, which reads as a
         * broken page rather than a young one.
         */
        int leftmost = W, rightmost = 0;
        for (int y = 0; y < H; y++)
            for (int xx = 0; xx < W; xx++)
                if (c.fb[y * W + xx] == PAL_A0) {
                    if (xx < leftmost) leftmost = xx;
                    if (xx > rightmost) rightmost = xx;
                }
        expect("and they use the width of the panel", rightmost - leftmost > W / 2);
    }

    /* A panel with no room below the header is left alone, not overrun. */
    {
        static uint16_t small[8 + 64 * 30 + 8];
        canvas_t c;
        memset(small, 0xAB, sizeof small);
        canvas_init(&c, small + 8, 64, 30, 1);
        drift_init(&d, 15);
        for (int i = 0; i < 30; i++) drift_add(&d, (float)i, true, 24.0f);
        drift_draw(&d, &c, 3);
        int intact = 1;
        for (int i = 0; i < 8; i++)
            if (small[i] != 0xABAB || small[8 + 64 * 30 + i] != 0xABAB) intact = 0;
        expect("a panel too short to chart is left alone, not overrun", intact);
    }

    printf("%s\n", failures ? "FAILURES" : "all tests passed");
    return failures ? 1 : 0;
}
