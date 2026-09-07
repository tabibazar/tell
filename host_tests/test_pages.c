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
             | PAGE_BIT(PAGE_THINKING) | PAGE_BIT(PAGE_WEEK) \
             | PAGE_BIT(PAGE_RECORDS) | PAGE_BIT(PAGE_RUNS))
#define FEATHER (PAGE_BIT(PAGE_CLOCK) | PAGE_BIT(PAGE_MESSAGE))

int main(void)
{
    pages_t p;

    pages_init(&p, ALL);
    expect("starts on the clock", p.current == PAGE_CLOCK);
    expect("advance goes to now", pages_advance(&p, 900) == PAGE_NOW);
    expect("then the week", pages_advance(&p, 950) == PAGE_WEEK);
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
    expect("then message", pages_advance(&p, 4800) == PAGE_MESSAGE);
    expect("then daily", pages_advance(&p, 4900) == PAGE_DAILY);
    expect("then settings, last of all", pages_advance(&p, 4950) == PAGE_SETTINGS);
    expect("then wraps to clock", pages_advance(&p, 5000) == PAGE_CLOCK);

    pages_init(&p, ALL);
    expect("back from the clock wraps to the last page", pages_back(&p, 100) == PAGE_SETTINGS);
    expect("back again", pages_back(&p, 200) == PAGE_DAILY);
    expect("back is activity", p.last_activity_us == 200);
    pages_show(&p, PAGE_STATS, 300);
    expect("back from stats skips nothing", pages_back(&p, 400) == PAGE_WEEK);
    pages_init(&p, FEATHER);
    expect("feather back skips absent pages", pages_back(&p, 500) == PAGE_MESSAGE);

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

    /* Rotation, which exists to stop a static image burning in. Compiled out
       when PAGES_ROTATE_US is zero, so the assertions follow the setting. */
#if PAGES_ROTATE_US > 0
    pages_init(&p, ALL);
    expect("no rotation while freshly pinned", !pages_tick(&p, 1000));

    int64_t t = PAGES_IDLE_US + PAGES_ROTATE_US + 1;
    expect("rotates once idle", pages_tick(&p, t));
    expect("moved off the clock", p.current == PAGE_NOW);
    expect("does not rotate again immediately", !pages_tick(&p, t + 1));
    expect("rotates after the interval", pages_tick(&p, t + PAGES_ROTATE_US + 1));
    expect("advanced again", p.current == PAGE_DAILY);

    /* Rotation must not count as activity, or it would pin itself and stop. */
    expect("rotation keeps rotating",
           pages_tick(&p, t + 2 * PAGES_ROTATE_US + 2));
    expect("still advancing", p.current == PAGE_MESSAGE);

    /* A touch pins the page again and suspends rotation. */
    int64_t touched = t + 3 * PAGES_ROTATE_US;
    pages_advance(&p, touched);
    expect("touch suspends rotation",
           !pages_tick(&p, touched + PAGES_ROTATE_US + 1));
    expect("rotation resumes after the pin expires",
           pages_tick(&p, touched + PAGES_IDLE_US + PAGES_ROTATE_US + 1));

    /* A board with one page has nothing to rotate to. */
    pages_init(&p, PAGE_BIT(PAGE_CLOCK));
    expect("a lone page reports no change",
           !pages_tick(&p, PAGES_IDLE_US + PAGES_ROTATE_US + 1));
#else
    pages_init(&p, ALL);
    expect("rotation disabled: never advances on its own",
           !pages_tick(&p, PAGES_IDLE_US * 100));
    expect("and the page does not move", p.current == PAGE_CLOCK);
#endif

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
    pages_show(&p, PAGE_RECORDS, 1000);
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
