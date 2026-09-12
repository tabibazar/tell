#include "pages.h"

#include <stdio.h>

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

#define ALL (PAGE_BIT(PAGE_CLOCK) | PAGE_BIT(PAGE_STATS) \
             | PAGE_BIT(PAGE_DAILY) | PAGE_BIT(PAGE_MESSAGE) \
             | PAGE_BIT(PAGE_TODAY) | PAGE_BIT(PAGE_MODELS) \
             | PAGE_BIT(PAGE_YEAR) | PAGE_BIT(PAGE_COST) \
             | PAGE_BIT(PAGE_SETTINGS) | PAGE_BIT(PAGE_NOW) \
             | PAGE_BIT(PAGE_RHYTHM) | PAGE_BIT(PAGE_PROJECTS) \
             | PAGE_BIT(PAGE_CACHE) | PAGE_BIT(PAGE_TOOLS) \
             | PAGE_BIT(PAGE_THINKING) | PAGE_BIT(PAGE_LIMITS) \
             | PAGE_BIT(PAGE_RECORDS) | PAGE_BIT(PAGE_RUNS) \
             | PAGE_BIT(PAGE_MENU) | PAGE_BIT(PAGE_TURNS) | PAGE_BIT(PAGE_STORY))
#define FEATHER (PAGE_BIT(PAGE_CLOCK) | PAGE_BIT(PAGE_MESSAGE))

int main(void)
{
    pages_t p;

    pages_init(&p, ALL);
    expect("starts on the clock", p.current == PAGE_CLOCK);
    expect("advance goes to the menu", pages_advance(&p, 850) == PAGE_MENU);
    expect("then now", pages_advance(&p, 900) == PAGE_NOW);
    expect("then the story", pages_advance(&p, 920) == PAGE_STORY);
    expect("then the limits", pages_advance(&p, 950) == PAGE_LIMITS);
    expect("then stats", pages_advance(&p, 1000) == PAGE_STATS);
    expect("then today", pages_advance(&p, 2000) == PAGE_TODAY);
    expect("then models", pages_advance(&p, 3000) == PAGE_MODELS);
    expect("then projects", pages_advance(&p, 3500) == PAGE_PROJECTS);
    expect("then the year", pages_advance(&p, 4000) == PAGE_YEAR);
    expect("then the rhythm", pages_advance(&p, 4200) == PAGE_RHYTHM);
    expect("then the cost", pages_advance(&p, 4500) == PAGE_COST);
    expect("then the cache", pages_advance(&p, 4600) == PAGE_CACHE);
    expect("then tools", pages_advance(&p, 4650) == PAGE_TOOLS);
    expect("then what it runs", pages_advance(&p, 4680) == PAGE_RUNS);
    expect("then thinking", pages_advance(&p, 4700) == PAGE_THINKING);
    expect("then records", pages_advance(&p, 4750) == PAGE_RECORDS);
    expect("then turns", pages_advance(&p, 4780) == PAGE_TURNS);
    expect("then message", pages_advance(&p, 4800) == PAGE_MESSAGE);
    expect("then daily", pages_advance(&p, 4900) == PAGE_DAILY);
    expect("then settings, last of all", pages_advance(&p, 4950) == PAGE_SETTINGS);
    expect("then wraps to clock", pages_advance(&p, 5000) == PAGE_CLOCK);

    pages_init(&p, ALL);
    expect("back from the clock wraps to the last page", pages_back(&p, 100) == PAGE_SETTINGS);
    expect("back again", pages_back(&p, 200) == PAGE_DAILY);
    expect("back is activity", p.last_activity_us == 200);
    pages_show(&p, PAGE_STATS, 300);
    expect("back from stats skips nothing", pages_back(&p, 400) == PAGE_LIMITS);
    pages_init(&p, FEATHER);
    expect("feather back skips absent pages", pages_back(&p, 500) == PAGE_MESSAGE);

    /* The Feather with its IMU: the clock, the message, the sand and the
       level. */
    pages_init(&p, FEATHER | PAGE_BIT(PAGE_PARTICLES) | PAGE_BIT(PAGE_LEVEL));
    expect("feather+imu starts on the clock", p.current == PAGE_CLOCK);
    pages_show(&p, PAGE_LEVEL, 1000);
    expect("it can be sent to the level", p.current == PAGE_LEVEL);
    pages_show(&p, PAGE_PARTICLES, 2000);
    expect("and to the sand", p.current == PAGE_PARTICLES);

    /* The level is where a board with the sensor comes up: it is the only
       reason that board has a screen. main picks the home page, but the
       availability this depends on is here. */
    pages_init(&p, FEATHER | PAGE_BIT(PAGE_LEVEL));
    expect("the level is available when the sensor is",
           (p.available & PAGE_BIT(PAGE_LEVEL)) != 0);

    /* A board without the sensor never lands on either. */
    pages_init(&p, FEATHER);
    pages_show(&p, PAGE_PARTICLES, 1000);
    expect("sand ignored when unavailable", p.current == PAGE_CLOCK);
    pages_show(&p, PAGE_LEVEL, 1000);
    expect("level ignored when unavailable", p.current == PAGE_CLOCK);

    pages_init(&p, FEATHER);
    expect("feather starts on the clock", p.current == PAGE_CLOCK);
    expect("feather skips absent pages", pages_advance(&p, 1000) == PAGE_MESSAGE);
    expect("feather wraps", pages_advance(&p, 2000) == PAGE_CLOCK);

    pages_init(&p, PAGE_BIT(PAGE_CLOCK));
    expect("a lone page stays put", pages_advance(&p, 1000) == PAGE_CLOCK);

    pages_init(&p, ALL);
    pages_show(&p, PAGE_DAILY, 5000);
    expect("show jumps to a page", p.current == PAGE_DAILY);

    pages_init(&p, FEATHER);
    pages_show(&p, PAGE_STATS, 7000);
    expect("show ignores an unavailable page", p.current == PAGE_CLOCK);

    pages_init(&p, ALL);
    pages_show(&p, PAGE_STATS, 0);
    expect("not idle immediately", !pages_idle_expired(&p, 1000));
    expect("not idle just under the limit",
           !pages_idle_expired(&p, PAGES_IDLE_US - 1));
    expect("idle past the limit", pages_idle_expired(&p, PAGES_IDLE_US + 1));
    pages_advance(&p, PAGES_IDLE_US + 1);
    expect("activity resets the idle timer",
           !pages_idle_expired(&p, PAGES_IDLE_US + 2));

    pages_init(&p, ALL);
    expect("the clock is never idle", !pages_idle_expired(&p, PAGES_IDLE_US * 10));

    /*
     * Rotation. It used to be a compile-time constant fixed at zero, so this
     * block never ran; it is a per-board setting now, and a board that has it
     * is read by watching rather than by pressing.
     */
#define ROT (20 * 1000000LL)
    pages_init(&p, ALL);
    expect("rotation is off unless a board asks for it",
           !pages_tick(&p, PAGES_IDLE_US * 100, 0));
    expect("and the page does not move", p.current == PAGE_CLOCK);

    pages_set_rotate(&p, ROT);
    expect("no rotation while freshly pinned", !pages_tick(&p, 1000, 0));

    /* The pin is a minute, not the five that hold the screensaver off: the
       point of a rotating board is that it keeps moving. */
    expect("still pinned just under the minute",
           !pages_tick(&p, PAGES_ROTATE_PIN_US - 1, 0));

    int64_t t = PAGES_ROTATE_PIN_US + ROT + 1;
    expect("rotates once the pin expires", pages_tick(&p, t, 0));
    expect("moved off the clock", p.current == PAGE_MENU);
    expect("does not rotate again immediately", !pages_tick(&p, t + 1, 0));
    expect("rotates after the interval", pages_tick(&p, t + ROT + 1, 0));

    /* Rotation must not count as activity, or it would pin itself and stop. */
    expect("rotation keeps rotating", pages_tick(&p, t + 2 * ROT + 2, 0));

    /* A button or a touch pins the page again and suspends rotation. */
    int64_t touched = t + 3 * ROT;
    pages_advance(&p, touched);
    expect("a press suspends rotation",
           !pages_tick(&p, touched + ROT + 1, 0));
    expect("rotation resumes a minute later",
           pages_tick(&p, touched + PAGES_ROTATE_PIN_US + ROT + 1, 0));

    /* The skip mask is the screensaver's, so a page struck off the round is
       off it here too. */
    {
        pages_t q;
        pages_init(&q, PAGE_BIT(PAGE_CLOCK) | PAGE_BIT(PAGE_MENU) | PAGE_BIT(PAGE_MESSAGE));
        pages_set_rotate(&q, ROT);
        int64_t u = PAGES_ROTATE_PIN_US + ROT + 1;
        expect("it rotates past a skipped page",
               pages_tick(&q, u, PAGE_BIT(PAGE_MENU)));
        expect("landing on the one that was not skipped", q.current == PAGE_MESSAGE);
    }

    /* A rotating board is its own screensaver; running both would be two
       slideshows fighting over the timing. */
    pages_init(&p, ALL);
    expect("a still board still gets a saver",
           pages_saver_active(&p, PAGES_SAVER_US + 1));
    pages_set_rotate(&p, ROT);
    expect("a rotating one does not",
           !pages_saver_active(&p, PAGES_SAVER_US + 1));

    /* A board with one page has nothing to rotate to. */
    pages_init(&p, PAGE_BIT(PAGE_CLOCK));
    pages_set_rotate(&p, ROT);
    expect("a lone page reports no change",
           !pages_tick(&p, PAGES_ROTATE_PIN_US + ROT + 1, 0));
#undef ROT

    /* Screensaver: on after PAGES_SAVER_US of nothing, off the moment
       anything happens. The assertions track the constant, not a fixed
       delay, so changing it does not silently invalidate them. */
    pages_init(&p, ALL);
    pages_show(&p, PAGE_CLOCK, 1000);
    expect("saver off straight after activity",
           !pages_saver_active(&p, 1000 + 1));
    expect("saver off just under the delay",
           !pages_saver_active(&p, 1000 + PAGES_SAVER_US));
    expect("saver on past the delay",
           pages_saver_active(&p, 1000 + PAGES_SAVER_US + 1));

    pages_advance(&p, 2000000);
    expect("a touch dismisses the saver", !pages_saver_active(&p, 2000001));
    expect("and it returns after the delay",
           pages_saver_active(&p, 2000000 + PAGES_SAVER_US + 1));

    /* The delay is a setting. */
    expect("the default delay is the constant", p.saver_us == PAGES_SAVER_US);
    pages_set_saver(&p, 60 * 1000000LL);
    expect("a shorter delay brings the saver sooner",
           pages_saver_active(&p, 2000000 + 60 * 1000000LL + 1)
           && !pages_saver_active(&p, 2000000 + 60 * 1000000LL));
    pages_set_saver(&p, 0);
    expect("zero means never", !pages_saver_active(&p, 2000000 + PAGES_SAVER_US * 100));
    pages_set_saver(&p, -5);
    expect("a negative delay is treated as never",
           p.saver_us == 0 && !pages_saver_active(&p, 2000000 + PAGES_SAVER_US * 100));

    /* Stepping, which the cycling saver uses: it moves without waking. */
    pages_init(&p, ALL);
    pages_show(&p, PAGE_TURNS, 1000);
    expect("step moves to the next page",
           pages_step(&p, 0) == PAGE_MESSAGE);
    expect("step is not activity", p.last_activity_us == 1000);
    expect("step skips masked pages",
           pages_step(&p, PAGE_BIT(PAGE_DAILY) | PAGE_BIT(PAGE_SETTINGS)) == PAGE_CLOCK);
    expect("step with everything masked stays put",
           pages_step(&p, ALL) == PAGE_CLOCK);

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
