#include "templog.h"
#include "palette.h"

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
    templog_t t;
    templog_init(&t, 300);

    float lo, hi;
    expect("an empty log has no range", !templog_range(&t, true, &lo, &hi));
    expect("and nothing in it", templog_count(&t) == 0);

    templog_add(&t, 50.0f, true, 24.0f);
    templog_add(&t, 52.0f, true, 24.5f);
    templog_add(&t, 51.0f, true, 24.25f);
    expect("three samples", templog_count(&t) == 3);
    expect("oldest first", templog_die(&t, 0) == 50.0f);
    expect("newest last", templog_die(&t, 2) == 51.0f);
    /* Each series has its own range: a shared one would set the scale from
       the gap between them and flatten what each actually does. */
    expect("the die has its own range",
           templog_range(&t, true, &lo, &hi) && lo == 50.0f && hi == 52.0f);
    expect("and the crystal has its own",
           templog_range(&t, false, &lo, &hi) && lo == 24.0f && hi == 24.5f);
    expect("the span is the samples times the interval", templog_span_s(&t) == 600);

    /* The ring: once full the oldest falls off the left, which is what makes
       it a rolling window rather than a log that stops. */
    templog_init(&t, 300);
    for (int i = 0; i < TEMPLOG_MAX + 25; i++)
        templog_add(&t, (float)i, true, 20.0f);
    expect("it fills and stops growing", templog_count(&t) == TEMPLOG_MAX);
    expect("the oldest has rolled off", templog_die(&t, 0) == 25.0f);
    expect("and the newest is the last one in",
           templog_die(&t, TEMPLOG_MAX - 1) == (float)(TEMPLOG_MAX + 24));

    /* A board with no clock chip still charts its own die. */
    templog_init(&t, 300);
    templog_add(&t, 55.0f, false, 0.0f);
    float x;
    expect("a missing crystal reads as missing", !templog_xtal(&t, 0, &x));
    expect("but the die is still there", templog_die(&t, 0) == 55.0f);
    expect("and there is still a die range", templog_range(&t, true, &lo, &hi));
    expect("but no crystal range at all", !templog_range(&t, false, &lo, &hi));

    /* A flat trace must not divide by zero when scaled, nor be drawn hard
       against one edge, which would read as a fault rather than steadiness. */
    templog_init(&t, 300);
    for (int i = 0; i < 20; i++) templog_add(&t, 42.0f, true, 42.0f);
    expect("a flat log still has a range", templog_range(&t, true, &lo, &hi));
    expect("which is not zero wide", hi > lo);
    expect("and is centred on the value", lo < 42.0f && hi > 42.0f);

    /*
     * A narrow-moving die must use the height it has. This is the whole
     * reason each series is scaled to itself: shared with a crystal 27
     * degrees away, half a degree of movement is a flat line.
     */
    {
        static uint16_t fb[8 + W * H + 8];
        canvas_t c;
        memset(fb, 0, sizeof fb);
        canvas_init(&c, fb + 8, W, H, 1);
        templog_init(&t, 300);
        for (int i = 0; i < 80; i++)
            templog_add(&t, 50.0f + (float)(i % 2) * 0.5f, true, 24.0f);
        templog_draw(&t, &c);
        int hi_row = H, lo_row = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (c.fb[y * W + x] == PAL_A1) {
                    if (y < hi_row) hi_row = y;
                    if (y > lo_row) lo_row = y;
                }
        expect("half a degree still spans the panel", lo_row - hi_row > H / 2);
    }

    /*
     * The chart writes only inside the framebuffer. This is the assertion
     * that matters: a chart scales user data into pixels, and one bad sample
     * or an empty log is exactly how that ends up off the end of the buffer.
     */
    {
        static uint16_t guarded[8 + W * H + 8];
        canvas_t c;
        int intact = 1;

        /* Empty, one sample, full, flat, and wildly out of range. */
        float cases[5] = { 0.0f, 1.0f, 100.0f, -300.0f, 1e9f };
        for (int k = 0; k < 5; k++) {
            memset(guarded, 0xAB, sizeof guarded);
            canvas_init(&c, guarded + 8, W, H, 1);
            templog_init(&t, 300);
            for (int i = 0; i < (k == 0 ? 0 : TEMPLOG_MAX + 5); i++)
                templog_add(&t, cases[k] + (float)(i % 7), true, cases[k] - 20.0f);
            templog_draw(&t, &c);
            for (int i = 0; i < 8; i++)
                if (guarded[i] != 0xABAB || guarded[8 + W * H + i] != 0xABAB)
                    intact = 0;
        }
        expect("the chart never draws outside the framebuffer", intact);

        /* And it actually draws something, so the guard is not passing on an
           empty canvas. */
        memset(guarded, 0, sizeof guarded);
        canvas_init(&c, guarded + 8, W, H, 1);
        templog_init(&t, 300);
        for (int i = 0; i < 60; i++)
            templog_add(&t, 50.0f + (float)(i % 9), true, 24.0f);
        templog_draw(&t, &c);
        int lit = 0;
        for (int i = 0; i < W * H; i++) if (c.fb[i] != 0) lit++;
        expect("and it does draw a chart", lit > 500);

        /* The bars stand under the line rather than instead of it: both
           colours must be present, and the bars must reach the floor. */
        int bars = 0, line = 0;
        for (int i = 0; i < W * H; i++) {
            if (c.fb[i] == pal_darken(PAL_A1)) bars++;
            if (c.fb[i] == PAL_A1) line++;
        }
        expect("the bars are drawn", bars > 500);
        expect("and the line survives on top of them", line > 50);
        {
            int floor_lit = 0;
            for (int x = 0; x < W; x++)
                if (c.fb[(H - 2) * W + x] != 0) floor_lit++;
            expect("the bars stand on the floor of the plot", floor_lit > W / 4);
        }
    }

    /* A tiny panel must be refused rather than drawn on badly. */
    {
        static uint16_t small[8 + 64 * 30 + 8];
        canvas_t c;
        memset(small, 0xAB, sizeof small);
        canvas_init(&c, small + 8, 64, 30, 1);
        templog_init(&t, 300);
        for (int i = 0; i < 30; i++) templog_add(&t, 50.0f, true, 24.0f);
        templog_draw(&t, &c);
        int intact = 1;
        for (int i = 0; i < 8; i++)
            if (small[i] != 0xABAB || small[8 + 64 * 30 + i] != 0xABAB) intact = 0;
        expect("a panel too short to chart is left alone, not overrun", intact);
    }

    printf("%s\n", failures ? "FAILURES" : "all tests passed");
    return failures ? 1 : 0;
}
