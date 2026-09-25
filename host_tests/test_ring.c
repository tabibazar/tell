#include "ring.h"

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

static int near(float a, float b, float tol)
{
    return fabsf(a - b) <= tol;
}

static int byte_near(int a, int b)
{
    return a - b <= 1 && b - a <= 1;
}

/*
 * The gradient's four stops in linear light, worked out by hand rather than
 * with the code's own formula, so a test cannot pass by repeating a mistake:
 * (c/255)^2.2 for each byte of green (0,200,60), yellow (255,200,0), orange
 * (255,110,0) and red (255,0,0).
 */
#define LIN200  0.585970f
#define LIN110  0.157286f
#define LIN60   0.041443f

/* The level where the fill stands at exactly n LEDs: 35 dBA plus n sevenths
   of the 50 dB span. */
static float laf_at(float n)
{
    return 35.0f + 50.0f * n / 7.0f;
}

/* One frame, with the frame's inputs spelled out at the call. */
static void frame(ring_t *r, float laf, float laeq, bool valid, bool night,
                  bool enabled, float dt, ring_rgb_t out[RING_LEDS])
{
    ring_in_t in = {
        .laf_dba = laf, .laeq3_dba = laeq, .dt_s = dt,
        .valid = valid, .night = night, .enabled = enabled,
    };
    ring_frame(r, &in, out);
}

/* A frame long enough that the glide lands on its target in one step: ten
   seconds is some sixty time constants. */
#define SNAP 10.0f
/* The LED task's own frame, 50 Hz. */
#define TICK 0.02f

static void snap(ring_t *r, float laf, float laeq, bool night, ring_rgb_t out[RING_LEDS])
{
    frame(r, laf, laeq, true, night, true, SNAP, out);
}

static int lit_count(const ring_rgb_t out[RING_LEDS])
{
    int n = 0;
    for (int i = 0; i < RING_LEDS; i++)
        if (out[i].r || out[i].g || out[i].b) n++;
    return n;
}

static int all_dark(const ring_rgb_t out[RING_LEDS])
{
    return lit_count(out) == 0;
}

static int same_rgb(ring_rgb_t a, ring_rgb_t b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

/* A ring at full drive, so the bytes are the gradient itself and a fraction
   of an LED is a fraction of 255, not of 64. */
static void init_full(ring_t *r)
{
    ring_init(r);
    r->cfg.day_cap = 1.0f;
    r->cfg.night_cap = 1.0f;
}

static void test_defaults(void)
{
    ring_t r;
    ring_init(&r);
    expect("default day cap is 25 %", r.cfg.day_cap == 0.25f);
    expect("default night cap is 8 %", r.cfg.night_cap == 0.08f);
    expect("default: index 0 is the quietest LED, running up the chain",
           r.cfg.first == 0 && !r.cfg.reverse);
    expect("a new ring starts dark-quiet: nothing filled, colour green",
           r.fill == 0.0f && r.colour_dba == 45.0f);
}

/* ---- the fill: LAF 35..85 dBA across seven LEDs ---- */

static void test_fill_target(void)
{
    expect("fill: 35 dBA is no LEDs", ring_fill_for(35.0f) == 0.0f);
    expect("fill: 85 dBA is all seven", near(ring_fill_for(85.0f), 7.0f, 1e-5f));
    expect("fill: 60 dBA, the midpoint, is three and a half",
           near(ring_fill_for(60.0f), 3.5f, 1e-5f));
    expect("fill: 40 dBA is 0.7 of the first LED", near(ring_fill_for(40.0f), 0.7f, 1e-5f));
    expect("fill: below 35 clamps to none", ring_fill_for(30.0f) == 0.0f);
    expect("fill: above 85 clamps to seven", ring_fill_for(100.0f) == 7.0f);
    expect("fill: silence (-inf dBA, log of zero) is none", ring_fill_for(-INFINITY) == 0.0f);
    expect("fill: +inf is seven", ring_fill_for(INFINITY) == 7.0f);
    expect("fill: NaN is none, not NaN", ring_fill_for(NAN) == 0.0f);

    int edges_ok = 1;
    for (int n = 0; n <= 7; n++)
        if (!near(ring_fill_for(laf_at((float)n)), (float)n, 1e-4f)) edges_ok = 0;
    expect("fill: every LED's edge, 35 + 50n/7 dBA, is exactly n LEDs", edges_ok);
}

static void test_fill_bytes(void)
{
    ring_t r;
    ring_rgb_t out[RING_LEDS];

    /* Red colour throughout, so each LED's amount is its R byte alone. */
    init_full(&r);
    snap(&r, 85.0f, 80.0f, false, out);
    int all_full = 1;
    for (int i = 0; i < RING_LEDS; i++)
        if (out[i].r != 255 || out[i].g || out[i].b) all_full = 0;
    expect("85 dBA lights all seven at full red", all_full);

    init_full(&r);
    snap(&r, 60.0f, 80.0f, false, out);
    expect("60 dBA: the first three full", out[0].r == 255 && out[1].r == 255 && out[2].r == 255);
    /* Half an LED is half the brightness to the eye: 0.5^2.2 = 0.2176 of the
       drive, 55 of 255. A plain half would be 128 and look nearly full. */
    printf("     half an LED at full drive: %d (0.5^2.2 * 255 = 55.5)\n", out[3].r);
    expect("60 dBA: the fourth half-lit to the eye, 0.5^2.2 of the drive",
           byte_near(out[3].r, 55));
    expect("60 dBA: the last three dark", !out[4].r && !out[5].r && !out[6].r);

    init_full(&r);
    snap(&r, laf_at(2.25f), 80.0f, false, out);
    expect("a quarter into the third LED: 0.25^2.2 of the drive, 12",
           out[0].r == 255 && out[1].r == 255 && byte_near(out[2].r, 12) && !out[3].r);

    init_full(&r);
    snap(&r, laf_at(1.0f), 80.0f, false, out);
    expect("exactly one LED's worth lights one LED, and only it",
           out[0].r == 255 && lit_count(out) == 1);
}

/* ---- below 35 dBA: one dim LED, the monitor is alive ---- */

static void test_heartbeat(void)
{
    ring_t r;
    ring_rgb_t out[RING_LEDS];

    init_full(&r);
    snap(&r, 30.0f, 80.0f, false, out);
    printf("     heartbeat at full drive, red: %d (0.3^2.2 * 255 = 18.0)\n", out[0].r);
    expect("30 dBA: a single LED lit", lit_count(out) == 1 && out[0].r > 0);
    expect("30 dBA: it is dim, 0.3 to the eye (18 of 255)", byte_near(out[0].r, 18));

    ring_rgb_t quiet = out[0];
    init_full(&r);
    snap(&r, 36.0f, 80.0f, false, out);
    expect("36 dBA, a seventh of the first LED, still shows the heartbeat, not less",
           same_rgb(out[0], quiet) && lit_count(out) == 1);

    init_full(&r);
    snap(&r, 40.0f, 80.0f, false, out);
    expect("40 dBA, 0.7 of the first LED, rises above the heartbeat",
           out[0].r > quiet.r && lit_count(out) == 1);

    init_full(&r);
    snap(&r, -INFINITY, 80.0f, false, out);
    expect("digital silence (-inf) is the heartbeat too", same_rgb(out[0], quiet) && lit_count(out) == 1);
}

/* ---- the colour: LAeq,3s through green, yellow, orange, red ---- */

static int lin_is(const float lin[3], float r, float g, float b)
{
    return near(lin[0], r, 1e-4f) && near(lin[1], g, 1e-4f) && near(lin[2], b, 1e-4f);
}

static void test_colour_target(void)
{
    float lin[3];

    ring_colour_for(45.0f, lin);
    expect("colour: 45 dBA is green (0,200,60), in linear light", lin_is(lin, 0.0f, LIN200, LIN60));
    ring_colour_for(55.0f, lin);
    expect("colour: 55 dBA is yellow (255,200,0)", lin_is(lin, 1.0f, LIN200, 0.0f));
    ring_colour_for(65.0f, lin);
    expect("colour: 65 dBA is orange (255,110,0)", lin_is(lin, 1.0f, LIN110, 0.0f));
    ring_colour_for(75.0f, lin);
    expect("colour: 75 dBA is red (255,0,0)", lin_is(lin, 1.0f, 0.0f, 0.0f));

    ring_colour_for(20.0f, lin);
    expect("colour: below 45 stays green", lin_is(lin, 0.0f, LIN200, LIN60));
    ring_colour_for(-INFINITY, lin);
    expect("colour: silence (-inf) is green", lin_is(lin, 0.0f, LIN200, LIN60));
    ring_colour_for(95.0f, lin);
    expect("colour: above 75 stays red", lin_is(lin, 1.0f, 0.0f, 0.0f));

    /* Midpoints are the mean of their two stops in LINEAR light. At 60 dBA
       that puts green at 0.372 (95 of 255); blending the sRGB bytes instead
       would give 155, which decodes to 0.335 (85) -- a dimmer, muddier
       orange-yellow. */
    ring_colour_for(50.0f, lin);
    expect("colour: 50 dBA is the linear mean of green and yellow",
           lin_is(lin, 0.5f, LIN200, LIN60 / 2.0f));
    ring_colour_for(60.0f, lin);
    expect("colour: 60 dBA is the linear mean of yellow and orange",
           lin_is(lin, 1.0f, (LIN200 + LIN110) / 2.0f, 0.0f));
    ring_colour_for(70.0f, lin);
    expect("colour: 70 dBA is the linear mean of orange and red",
           lin_is(lin, 1.0f, LIN110 / 2.0f, 0.0f));
    ring_colour_for(NAN, lin);
    expect("colour: NaN gives a colour on the scale, not NaN",
           !isnan(lin[0]) && !isnan(lin[1]) && !isnan(lin[2]));

    /* Every channel moves one way only from green to red, so a glide along
       the level can never swing a channel past where it is going. */
    float prev[3];
    ring_colour_for(40.0f, prev);
    int mono = 1;
    for (float d = 40.1f; d <= 80.0f; d += 0.1f) {
        ring_colour_for(d, lin);
        if (lin[0] < prev[0] - 1e-6f || lin[1] > prev[1] + 1e-6f || lin[2] > prev[2] + 1e-6f)
            mono = 0;
        memcpy(prev, lin, sizeof prev);
    }
    expect("colour: red only rises and green and blue only fall, 40..80 dBA", mono);
}

static void test_colour_bytes(void)
{
    ring_t r;
    ring_rgb_t out[RING_LEDS];

    /* At full drive and a full ring, LED 0 shows the colour itself. */
    struct { float dba; int r, g, b; const char *name; } stops[] = {
        { 45.0f,   0, 149, 11, "green (0,200,60) -> (0,149,11)" },
        { 50.0f, 128, 149,  5, "50 dBA -> (128,149,5)" },
        { 55.0f, 255, 149,  0, "yellow (255,200,0) -> (255,149,0)" },
        { 60.0f, 255,  95,  0, "60 dBA -> (255,95,0)" },
        { 65.0f, 255,  40,  0, "orange (255,110,0) -> (255,40,0)" },
        { 70.0f, 255,  20,  0, "70 dBA -> (255,20,0)" },
        { 75.0f, 255,   0,  0, "red (255,0,0) -> (255,0,0)" },
    };
    for (size_t i = 0; i < sizeof stops / sizeof stops[0]; i++) {
        init_full(&r);
        snap(&r, 85.0f, stops[i].dba, false, out);
        char what[96];
        snprintf(what, sizeof what, "bytes at full drive: %s", stops[i].name);
        expect(what, byte_near(out[0].r, stops[i].r) && byte_near(out[0].g, stops[i].g)
                     && byte_near(out[0].b, stops[i].b));
    }
}

/* ---- the glide: exponential, the fill up in 40 ms and down in 150 ---- */

/* 1 - 1/e: how much of a step one time constant covers. */
#define ONE_TAU  0.632121f

static void test_glide(void)
{
    ring_t r;
    ring_rgb_t out[RING_LEDS];

    /* One time constant of each kind covers 63 % of its step. The fill goes
       up from 0.7 LED (40 dBA) toward 7, so 0.7 + 6.3 * 0.632; the colour
       from green's 45 toward red's 75, so 45 + 30 * 0.632 = 63.96 dBA. */
    init_full(&r);
    snap(&r, 40.0f, 40.0f, false, out);
    frame(&r, 85.0f, 75.0f, true, false, true, 0.04f, out);
    expect("glide: one rise time constant (40 ms) covers 63 % of the fill's step up",
           near(r.fill, 0.7f + 6.3f * ONE_TAU, 1e-3f));

    init_full(&r);
    snap(&r, 40.0f, 40.0f, false, out);
    frame(&r, 85.0f, 75.0f, true, false, true, 0.15f, out);
    expect("glide: one 150 ms time constant covers 63 % of the colour's step",
           near(r.colour_dba, 45.0f + 30.0f * ONE_TAU, 1e-3f));

    init_full(&r);
    snap(&r, 85.0f, 75.0f, false, out);
    frame(&r, 40.0f, 45.0f, true, false, true, 0.15f, out);
    expect("glide: one fall time constant (150 ms) covers 63 % of the fill's step down",
           near(r.fill, 7.0f - 6.3f * ONE_TAU, 1e-3f));
    expect("glide: and the colour's step down alike",
           near(r.colour_dba, 75.0f - 30.0f * ONE_TAU, 1e-3f));

    /* The same gap, one frame each way: up moves further than down. */
    init_full(&r);
    snap(&r, 40.0f, 40.0f, false, out);
    frame(&r, 85.0f, 40.0f, true, false, true, TICK, out);
    float up = r.fill - 0.7f;
    init_full(&r);
    snap(&r, 85.0f, 40.0f, false, out);
    frame(&r, 40.0f, 40.0f, true, false, true, TICK, out);
    float down = 7.0f - r.fill;
    printf("     one frame of a 6.3 LED step: up %.2f, down %.2f\n", (double)up, (double)down);
    expect("glide: a frame rises further than it falls", up > 2.0f * down);

    /* Upward, at the task's 50 Hz. */
    init_full(&r);
    snap(&r, 40.0f, 40.0f, false, out);
    float target = ring_fill_for(80.0f);
    float pf = r.fill, pc = r.colour_dba;
    int rising = 1, over = 0, fill_1pct = -1, colour_1pct = -1;
    for (int i = 1; i <= 200; i++) {
        frame(&r, 80.0f, 80.0f, true, false, true, TICK, out);
        if (r.fill < pf || r.colour_dba < pc) rising = 0;
        if (r.fill > target || r.colour_dba > 75.0f) over = 1;
        if (fill_1pct < 0 && target - r.fill <= 0.01f * (target - 0.7f)) fill_1pct = i;
        if (colour_1pct < 0 && 75.0f - r.colour_dba <= 0.01f * 30.0f) colour_1pct = i;
        pf = r.fill;
        pc = r.colour_dba;
    }
    printf("     within 1 %% of a step: fill after %d frames (5 rise tau = 10), colour after %d (5 tau = 37.5)\n",
           fill_1pct, colour_1pct);
    expect("glide up: fill and colour never fall back", rising);
    expect("glide up: never past the target, colour never past red's 75", !over);
    expect("glide up: the fill within 1 % after five rise time constants (10 frames)",
           fill_1pct > 0 && fill_1pct <= 10);
    expect("glide up: the colour within 1 % after five of its own (38 frames)",
           colour_1pct > 0 && colour_1pct <= 38);
    expect("glide up: converged after four seconds",
           near(r.fill, target, 1e-4f) && near(r.colour_dba, 75.0f, 1e-3f));

    /* And down, from loud to quiet. */
    pf = r.fill;
    pc = r.colour_dba;
    int falling = 1, under = 0;
    for (int i = 0; i < 200; i++) {
        frame(&r, 40.0f, 40.0f, true, false, true, TICK, out);
        if (r.fill > pf || r.colour_dba > pc) falling = 0;
        if (r.fill < 0.7f - 1e-6f || r.colour_dba < 45.0f) under = 1;
        pf = r.fill;
        pc = r.colour_dba;
    }
    expect("glide down: fill and colour never rise", falling);
    expect("glide down: never under the target, colour never under green's 45", !under);
    expect("glide down: converged", near(r.fill, 0.7f, 1e-4f) && near(r.colour_dba, 45.0f, 1e-3f));

    /* The colour glides toward the scale's end, not toward the raw level: a
       room at 95 dBA that drops to 60 turns from red at once, instead of
       first gliding down through twenty dB that all look the same red. */
    init_full(&r);
    snap(&r, 85.0f, 95.0f, false, out);
    expect("glide: a level past red holds the colour at red's 75, not at 95",
           r.colour_dba == 75.0f);

    /* A huge frame lands exactly on the target, not a rounding error past it. */
    init_full(&r);
    snap(&r, 40.0f, 40.0f, false, out);
    frame(&r, 80.0f, 60.0f, true, false, true, 1e6f, out);
    expect("glide: an enormous dt lands exactly on the target",
           r.fill == ring_fill_for(80.0f) && r.colour_dba == 60.0f);
    frame(&r, 40.0f, 50.0f, true, false, true, INFINITY, out);
    expect("glide: an infinite dt lands exactly on the target",
           r.fill == ring_fill_for(40.0f) && r.colour_dba == 50.0f);

    /* Frames with no time in them change nothing. */
    float f0 = r.fill, c0 = r.colour_dba;
    frame(&r, 85.0f, 75.0f, true, false, true, 0.0f, out);
    frame(&r, 85.0f, 75.0f, true, false, true, -1.0f, out);
    frame(&r, 85.0f, 75.0f, true, false, true, NAN, out);
    expect("glide: dt of 0, negative or NaN leaves the state alone",
           r.fill == f0 && r.colour_dba == c0);
}

/*
 * A clap: one loud 125 ms block over a quiet room. LAF holds it for that one
 * block, which is six or seven frames at 50 Hz depending on where the frames
 * fall, and the fill is meant to be "louder right now", so the ring has to
 * show it. Six frames (120 ms) of a 40 ms rise cover 1 - e^-3 = 95 % of the
 * way. One 150 ms time constant for everything, as the glide once was, got
 * 55 % of the way: 4.2 of 7 LEDs for a clap at 85 dBA over a 40 dBA room.
 * Afterwards the fill eases back at the fall's 150 ms rather than vanishing.
 */
static void test_clap(void)
{
    char what[112];

    for (int frames = 6; frames <= 7; frames++) {
        ring_t r;
        ring_rgb_t out[RING_LEDS], at_peak[RING_LEDS];
        float peak;

        init_full(&r);
        snap(&r, 40.0f, 40.0f, false, out);
        for (int i = 0; i < frames; i++) frame(&r, 85.0f, 40.0f, true, false, true, TICK, out);
        peak = r.fill;
        memcpy(at_peak, out, sizeof at_peak);

        int easing = 1;
        for (int k = 1; k <= 5; k++) {
            frame(&r, 40.0f, 40.0f, true, false, true, TICK, out);
            float want = 0.7f + (peak - 0.7f) * expf(-(float)k * TICK / 0.15f);
            if (!near(r.fill, want, 1e-3f)) easing = 0;
        }

        printf("     a clap held %d frames: peak fill %.2f of 7, %d LEDs lit\n",
               frames, (double)peak, lit_count(at_peak));
        snprintf(what, sizeof what, "clap, %d frames of 85 dBA over 40: at least 90 %% of the step shown", frames);
        expect(what, peak >= 0.7f + 0.9f * 6.3f);
        snprintf(what, sizeof what, "clap, %d frames: all seven LEDs lit at its peak", frames);
        expect(what, lit_count(at_peak) == 7);
        snprintf(what, sizeof what, "clap, %d frames: then eases back with the 150 ms fall", frames);
        expect(what, easing);
    }
}

/* ---- brightness: 25 % by day, 8 % by night, off is off ---- */

static void test_brightness(void)
{
    ring_t r;
    ring_rgb_t out[RING_LEDS];

    ring_init(&r);
    snap(&r, 85.0f, 80.0f, false, out);
    expect("day: full red is 25 % of the drive, 64", out[0].r == 64 && out[6].r == 64);

    ring_init(&r);
    snap(&r, 85.0f, 80.0f, true, out);
    expect("night: full red is 8 % of the drive, 20", out[0].r == 20 && out[6].r == 20);

    /* Night against day, for the same room, across the whole scale. */
    int never_brighter = 1, some_dimmer = 0;
    for (float laf = 30.0f; laf <= 90.0f; laf += 5.0f)
        for (float laeq = 40.0f; laeq <= 80.0f; laeq += 5.0f) {
            ring_t d, n;
            ring_rgb_t od[RING_LEDS], on[RING_LEDS];
            ring_init(&d);
            ring_init(&n);
            snap(&d, laf, laeq, false, od);
            snap(&n, laf, laeq, true, on);
            for (int i = 0; i < RING_LEDS; i++) {
                if (on[i].r > od[i].r || on[i].g > od[i].g || on[i].b > od[i].b) never_brighter = 0;
                if (on[i].r < od[i].r || on[i].g < od[i].g || on[i].b < od[i].b) some_dimmer = 1;
            }
        }
    expect("night is never brighter than day on any byte", never_brighter);
    expect("night is dimmer than day somewhere", some_dimmer);

    /* The caps are ceilings on the drive, not on perceived brightness. Read
       as perceived, 8 % would be 0.08^2.2 = 0.4 % of the drive, one count of
       255: orange and red would both be (1,0,0) and night would lose the
       scale it is meant to keep measuring on. */
    ring_rgb_t red, orange, yellow, green;
    ring_init(&r); snap(&r, 85.0f, 75.0f, true, out); red = out[0];
    ring_init(&r); snap(&r, 85.0f, 65.0f, true, out); orange = out[0];
    ring_init(&r); snap(&r, 85.0f, 55.0f, true, out); yellow = out[0];
    ring_init(&r); snap(&r, 85.0f, 45.0f, true, out); green = out[0];
    expect("night: orange still differs from red", !same_rgb(orange, red));
    expect("night: yellow still differs from orange", !same_rgb(yellow, orange));
    expect("night: green still differs from yellow", !same_rgb(green, yellow));

    printf("     full LED, day / night:\n");
    for (int k = 0; k < 4; k++) {
        float laeq = 45.0f + 10.0f * (float)k;
        ring_rgb_t od[RING_LEDS], on[RING_LEDS];
        ring_init(&r); snap(&r, 85.0f, laeq, false, od);
        ring_init(&r); snap(&r, 85.0f, laeq, true, on);
        printf("       %2.0f dBA  (%3d,%3d,%3d) / (%3d,%3d,%3d)\n", (double)laeq,
               od[0].r, od[0].g, od[0].b, on[0].r, on[0].g, on[0].b);
    }

    /* The heartbeat, dimmest by night: 0.08 * 0.07 of a full LED is under
       one count on green, and it must still show. */
    ring_rgb_t hd[RING_LEDS], hn[RING_LEDS], hr[RING_LEDS];
    ring_init(&r); snap(&r, 30.0f, 40.0f, false, hd);
    ring_init(&r); snap(&r, 30.0f, 40.0f, true, hn);
    ring_init(&r); snap(&r, 30.0f, 80.0f, true, hr);
    printf("     heartbeat, green day (%d,%d,%d), green night (%d,%d,%d), red night (%d,%d,%d)\n",
           hd[0].r, hd[0].g, hd[0].b, hn[0].r, hn[0].g, hn[0].b, hr[0].r, hr[0].g, hr[0].b);
    expect("heartbeat by day: one LED, lit", lit_count(hd) == 1 && (hd[0].g > 0));
    expect("heartbeat by night: one LED, still lit", lit_count(hn) == 1 && (hn[0].g > 0));
    expect("heartbeat by night in red: still lit", lit_count(hr) == 1 && hr[0].r > 0);

    /* Lower the ceiling until the heartbeat's green is a third of a count,
       0.02 * 0.07 * 0.586 * 255, which rounds to black. It must not. */
    ring_init(&r);
    r.cfg.night_cap = 0.02f;
    snap(&r, 30.0f, 40.0f, true, out);
    expect("a ceiling too low for the heartbeat still shows it, as one count of green",
           lit_count(out) == 1 && out[0].r == 0 && out[0].g == 1 && out[0].b == 0);
    snap(&r, laf_at(2.1f), 40.0f, true, out);
    expect("only the heartbeat is held up: a tenth of the third LED stays dark",
           out[1].g > 1 && out[2].g == 0 && out[2].b == 0);

    /* Off. */
    ring_init(&r);
    snap(&r, 85.0f, 80.0f, false, out);
    frame(&r, 85.0f, 80.0f, true, false, false, TICK, out);
    expect("off: all 21 bytes zero, on the very first frame", all_dark(out));
    frame(&r, 30.0f, 40.0f, true, false, false, TICK, out);
    expect("off: no heartbeat either", all_dark(out));
    frame(&r, 30.0f, 40.0f, false, true, false, TICK, out);
    expect("off: nothing at night or with no audio", all_dark(out));

    /* Off keeps measuring, so `!ring on` lands on the room as it is now. */
    ring_init(&r);
    snap(&r, 40.0f, 40.0f, false, out);
    for (int i = 0; i < 100; i++) frame(&r, 85.0f, 75.0f, true, false, false, TICK, out);
    expect("off: the state keeps gliding underneath",
           near(r.fill, 7.0f, 1e-3f) && near(r.colour_dba, 75.0f, 1e-3f));
    frame(&r, 85.0f, 75.0f, true, false, true, TICK, out);
    expect("on again: straight back to the full ring", lit_count(out) == 7 && out[6].r == 64);

    /* A cap of zero is dark, heartbeat and all. */
    ring_init(&r);
    r.cfg.day_cap = 0.0f;
    snap(&r, 30.0f, 40.0f, false, out);
    expect("a zero cap is dark, heartbeat included", all_dark(out));
}

/* ---- no audio yet, or gated: the heartbeat only ---- */

static void test_invalid(void)
{
    ring_t r;
    ring_rgb_t out[RING_LEDS];

    ring_init(&r);
    frame(&r, 85.0f, 80.0f, false, false, true, TICK, out);
    expect("no audio yet: the first frame is the heartbeat alone", lit_count(out) == 1 && out[0].g > 0);
    expect("no audio yet: in green, the colour a new ring starts in",
           out[0].r == 0 && out[0].g > out[0].b);

    /* Gated while loud: the fill glides down to the heartbeat and the colour
       holds, so a chime's gate over a red room does not flash green. */
    ring_init(&r);
    snap(&r, 85.0f, 80.0f, false, out);
    for (int i = 0; i < 50; i++) frame(&r, 85.0f, 80.0f, false, false, true, TICK, out);
    expect("gated for a second: only LED 0 lit", lit_count(out) == 1 && out[0].r > 0);
    expect("gated: at the heartbeat's dimness (red, 5 of 255 by day)", byte_near(out[0].r, 5));
    expect("gated: the colour held at red", r.colour_dba == 75.0f && out[0].g == 0);

    ring_rgb_t beat = out[0];
    ring_init(&r);
    snap(&r, 85.0f, 80.0f, false, out);
    frame(&r, NAN, 80.0f, true, false, true, SNAP, out);
    expect("a NaN LAF counts as no audio: heartbeat only", lit_count(out) == 1 && same_rgb(out[0], beat));
    frame(&r, 60.0f, NAN, true, false, true, SNAP, out);
    expect("a NaN LAeq counts as no audio too, colour held", r.colour_dba == 75.0f && lit_count(out) == 1);
}

/* ---- where index 0 sits: rotation and direction ---- */

static void test_rotation(void)
{
    ring_t r;
    ring_rgb_t out[RING_LEDS];

    init_full(&r);
    snap(&r, laf_at(2.0f), 80.0f, false, out);
    expect("default: two LEDs fill chain 0 and 1",
           out[0].r == 255 && out[1].r == 255 && lit_count(out) == 2);

    init_full(&r);
    r.cfg.first = 3;
    snap(&r, laf_at(2.0f), 80.0f, false, out);
    expect("first = 3: two LEDs fill chain 3 and 4",
           out[3].r == 255 && out[4].r == 255 && lit_count(out) == 2);

    init_full(&r);
    r.cfg.first = 3;
    r.cfg.reverse = true;
    snap(&r, laf_at(2.0f), 80.0f, false, out);
    expect("first = 3, reversed: chain 3 and 2",
           out[3].r == 255 && out[2].r == 255 && lit_count(out) == 2);

    init_full(&r);
    r.cfg.reverse = true;
    snap(&r, laf_at(2.0f), 80.0f, false, out);
    expect("first = 0, reversed: chain 0 and then 6, round the ring",
           out[0].r == 255 && out[6].r == 255 && lit_count(out) == 2);

    init_full(&r);
    r.cfg.first = 6;
    snap(&r, laf_at(3.0f), 80.0f, false, out);
    expect("first = 6: three LEDs wrap to chain 6, 0 and 1",
           out[6].r == 255 && out[0].r == 255 && out[1].r == 255 && lit_count(out) == 3);

    init_full(&r);
    r.cfg.first = 9;
    snap(&r, laf_at(2.0f), 80.0f, false, out);
    expect("first = 9 is taken round the ring, as 2",
           out[2].r == 255 && out[3].r == 255 && lit_count(out) == 2);

    init_full(&r);
    r.cfg.first = 4;
    snap(&r, 30.0f, 80.0f, false, out);
    expect("the heartbeat sits at `first` too", lit_count(out) == 1 && out[4].r > 0);
}

/* ---- whatever comes in, the output stays sane ---- */

static uint32_t seed = 12345u;
static float rnd(float lo, float hi)
{
    seed = seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * (float)(seed >> 8) / 16777216.0f;
}

static float wild(float lo, float hi)
{
    switch ((seed >> 4) % 11u) {
    case 0: rnd(0.0f, 1.0f); return NAN;
    case 1: rnd(0.0f, 1.0f); return INFINITY;
    case 2: rnd(0.0f, 1.0f); return -INFINITY;
    case 3: rnd(0.0f, 1.0f); return 1e9f;
    case 4: rnd(0.0f, 1.0f); return -1e9f;
    default: return rnd(lo, hi);
    }
}

static void test_range(void)
{
    ring_t r;
    ring_rgb_t out[RING_LEDS];
    ring_init(&r);

    int bounded = 1, state_ok = 1;
    for (int i = 0; i < 20000; i++) {
        if (i % 500 == 0) {
            r.cfg.day_cap = wild(-0.5f, 1.5f);
            r.cfg.night_cap = wild(-0.5f, 1.5f);
            r.cfg.first = (uint8_t)(seed >> 11);
            r.cfg.reverse = (seed >> 7) & 1u;
        }
        bool night = (seed >> 3) & 1u;
        float laf = wild(0.0f, 120.0f), laeq = wild(0.0f, 120.0f), dt = wild(-0.05f, 0.5f);
        bool valid = (seed >> 5) % 5u != 0;
        bool enabled = (seed >> 9) % 7u != 0;
        frame(&r, laf, laeq, valid, night, enabled, dt, out);

        /* No byte past the cap (rounded), bar the one count the heartbeat is
           allowed; a cap out of 0..1 or NaN is read as clamped. */
        float cap = night ? r.cfg.night_cap : r.cfg.day_cap;
        cap = isnan(cap) ? 0.0f : fminf(fmaxf(cap, 0.0f), 1.0f);
        int top = (int)(cap * 255.0f + 0.5f);
        if (top < 1 && cap > 0.0f) top = 1;
        if (!enabled) top = 0;
        for (int k = 0; k < RING_LEDS; k++)
            if (out[k].r > top || out[k].g > top || out[k].b > top) bounded = 0;
        if (!(r.fill >= 0.0f && r.fill <= 7.0f && r.colour_dba >= 45.0f && r.colour_dba <= 75.0f))
            state_ok = 0;
    }
    expect("20000 wild frames: no byte above the cap (no wrap-around)", bounded);
    expect("20000 wild frames: the state stays finite and on the scale", state_ok);
}

int main(void)
{
    test_defaults();
    test_fill_target();
    test_fill_bytes();
    test_heartbeat();
    test_colour_target();
    test_colour_bytes();
    test_glide();
    test_clap();
    test_brightness();
    test_invalid();
    test_rotation();
    test_range();
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("all passed\n");
    return 0;
}
