#include "shaketimer.h"

#include <stdio.h>

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

/* Frame steps the board actually uses, so rounding is measured rather than
   hidden behind one big jump. */
static void run(shaketimer_t *t, float secs)
{
    for (float s = 0; s < secs; s += 0.05f) shaketimer_tick(t, 0.05f);
}

/* A board sitting still: gravity down one axis and nothing else. Advances
   the caller's clock and returns how many shakes were reported. */
static int feed_still(shakedet_t *d, int64_t *us, float secs)
{
    int shakes = 0;
    for (float s = 0; s < secs; s += 0.05f) {
        *us += 50000;
        if (shakedet_update(d, 0.01f, -0.02f, 0.98f, 0.05f, *us)) shakes++;
    }
    return shakes;
}

int main(void)
{
    /* ---- the clock ---- */
    shaketimer_t t;
    shaketimer_init(&t, 60);
    expect("starts idle", shaketimer_state(&t) == ST_IDLE);
    expect("showing the whole time", shaketimer_remaining_s(&t) == 60.0f);
    run(&t, 5.0f);
    expect("idle does not count down", shaketimer_remaining_s(&t) == 60.0f);

    shaketimer_shake(&t);
    expect("a shake starts it", shaketimer_state(&t) == ST_RUNNING);
    run(&t, 20.0f);
    {
        float left = shaketimer_remaining_s(&t);
        expect("and it counts down", left > 39.0f && left < 41.0f);
    }

    /* The whole contract: a shake always means start again from the top. */
    shaketimer_shake(&t);
    expect("a shake mid-run resets it", shaketimer_remaining_s(&t) == 60.0f);
    expect("and leaves it running", shaketimer_state(&t) == ST_RUNNING);

    run(&t, 61.0f);
    expect("it finishes", shaketimer_state(&t) == ST_DONE);
    expect("with nothing left", shaketimer_remaining_s(&t) == 0.0f);
    run(&t, 30.0f);
    expect("and does not run past zero", shaketimer_remaining_s(&t) == 0.0f);

    shaketimer_shake(&t);
    expect("a shake restarts a finished one", shaketimer_state(&t) == ST_RUNNING);
    expect("from the top", shaketimer_remaining_s(&t) == 60.0f);

    shaketimer_init(&t, 0);
    shaketimer_shake(&t);
    run(&t, 1.0f);
    expect("a zero duration is done at once", shaketimer_state(&t) == ST_DONE);

    /* ---- setting it on the board ---- */
    shaketimer_init(&t, 300);
    expect("not setting to begin with", !shaketimer_setting(&t));

    shaketimer_begin_set(&t);
    expect("begins from the current duration", shaketimer_remaining_s(&t) == 300.0f);
    expect("and says it is setting", shaketimer_setting(&t));

    /* Two seconds of a full turn, at a minute per turn. */
    shaketimer_adjust(&t, 1.0f, 0.0f, 2.0f);
    expect("a turn winds the minutes on", shaketimer_remaining_s(&t) == 420.0f);
    shaketimer_adjust(&t, -1.0f, 0.0f, 1.0f);
    expect("and back the other way", shaketimer_remaining_s(&t) == 360.0f);
    shaketimer_adjust(&t, 0.0f, 10.0f, 1.5f);
    expect("tilt winds the seconds", shaketimer_remaining_s(&t) == 375.0f);

    /* A slow turn must accumulate rather than rounding away to nothing: this
       is why the dialled value is kept fractional. */
    for (int i = 0; i < 20; i++) shaketimer_adjust(&t, 0.0f, 0.4f, 0.05f);
    expect("a slow turn still moves it", shaketimer_remaining_s(&t) > 375.0f);

    shaketimer_adjust(&t, -100.0f, 0.0f, 60.0f);
    expect("it cannot be wound below the floor",
           shaketimer_remaining_s(&t) == (float)ST_MIN_S);
    shaketimer_adjust(&t, 1000.0f, 0.0f, 60.0f);
    expect("nor above the ceiling", shaketimer_remaining_s(&t) == (float)ST_MAX_S);

    /* A shake while dialling is an unsteady hand, not an instruction. */
    shaketimer_shake(&t);
    expect("a shake does not start it while setting", shaketimer_setting(&t));

    shaketimer_adjust(&t, -1.0f, 0.0f, 1.0f);    /* one minute back from the top */
    shaketimer_accept(&t);
    expect("accepting leaves it idle, not running", shaketimer_state(&t) == ST_IDLE);
    expect("with the dialled time", shaketimer_remaining_s(&t) == 5880.0f);
    shaketimer_shake(&t);
    run(&t, 1.0f);
    expect("and a shake then runs the new time", shaketimer_state(&t) == ST_RUNNING);

    /* Winding is only possible while setting. */
    shaketimer_adjust(&t, 10.0f, 0.0f, 10.0f);
    expect("adjust does nothing when not setting",
           shaketimer_remaining_s(&t) < 5880.0f);

    /* ---- the shuttle curve ---- */
    expect("still is still", shaketimer_shuttle(0.0f, 0.25f, 100.0f) == 0.0f);
    expect("inside the dead zone is still",
           shaketimer_shuttle(0.2f, 0.25f, 100.0f) == 0.0f);
    expect("and so is the other way",
           shaketimer_shuttle(-0.2f, 0.25f, 100.0f) == 0.0f);
    expect("it starts from zero at the edge of the dead zone",
           shaketimer_shuttle(0.2501f, 0.25f, 100.0f) < 0.01f);
    expect("held hard it reaches full rate",
           shaketimer_shuttle(1.0f, 0.25f, 100.0f) > 99.9f);
    expect("past full it does not exceed it",
           shaketimer_shuttle(4.0f, 0.25f, 100.0f) <= 100.0f);
    expect("the other way is the same speed backwards",
           shaketimer_shuttle(-1.0f, 0.25f, 100.0f) == -100.0f);
    {
        /* The point of squaring: a little tilt is much slower than half the
           speed, so small corrections are possible at all. */
        float half = shaketimer_shuttle(0.625f, 0.25f, 100.0f);  /* halfway up */
        expect("half held is well under half speed", half < 30.0f && half > 20.0f);
        float last = -1.0f, ok = 1.0f;
        for (float h = 0.25f; h <= 1.0f; h += 0.05f) {
            float r = shaketimer_shuttle(h, 0.25f, 100.0f);
            if (r < last) ok = 0.0f;
            last = r;
        }
        expect("and it never goes backwards as you lean further", ok != 0.0f);
    }

    /* ---- the detector ---- */
    shakedet_t d;
    shakedet_init(&d);
    int64_t us = 1000000;

    /* A still board must never report a shake, however long it sits there:
       a timer that resets itself on the desk would be worse than no timer. */
    expect("a still board never reports a shake", feed_still(&d, &us, 10.0f) == 0);

    /* A real shake: the reading swings far from where gravity was. */
    int shakes = 0;
    for (int i = 0; i < 6; i++) {
        us += 50000;
        float sign = (i % 2) ? 1.0f : -1.0f;
        if (shakedet_update(&d, sign * 2.4f, sign * -1.9f, 0.4f, 0.05f, us)) shakes++;
    }
    expect("a shake is recognised", shakes >= 1);
    expect("and only once, not once per sample", shakes == 1);

    /* Once it has settled again, another shake is a second shake. */
    for (int i = 0; i < 40; i++) { us += 50000; shakedet_update(&d, 0.01f, -0.02f, 0.98f, 0.05f, us); }
    int again = 0;
    for (int i = 0; i < 6; i++) {
        us += 50000;
        float sign = (i % 2) ? 1.0f : -1.0f;
        if (shakedet_update(&d, sign * 2.4f, sign * -1.9f, 0.4f, 0.05f, us)) again++;
    }
    expect("a later shake counts again", again == 1);

    /* Turning the board over slowly is not a shake, which is what keeps the
       thing usable: you can pick it up and look at it without resetting it. */
    shakedet_init(&d);
    int slow = 0;
    for (int i = 0; i < 60; i++) {
        us += 50000;
        float f = (float)i / 59.0f;        /* gravity swings across, gently */
        if (shakedet_update(&d, f * 0.98f, 0.0f, (1.0f - f) * 0.98f, 0.05f, us)) slow++;
    }
    expect("turning it over slowly is not a shake", slow == 0);

    printf("%s\n", failures ? "FAILURES" : "all tests passed");
    return failures ? 1 : 0;
}
