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
             | PAGE_BIT(PAGE_DAILY) | PAGE_BIT(PAGE_MESSAGE))
#define FEATHER (PAGE_BIT(PAGE_CLOCK) | PAGE_BIT(PAGE_MESSAGE))

int main(void)
{
    pages_t p;

    pages_init(&p, ALL);
    expect("starts on the clock", p.current == PAGE_CLOCK);
    expect("advance goes to stats", pages_advance(&p, 1000) == PAGE_STATS);
    expect("then daily", pages_advance(&p, 2000) == PAGE_DAILY);
    expect("then message", pages_advance(&p, 3000) == PAGE_MESSAGE);
    expect("then wraps to clock", pages_advance(&p, 4000) == PAGE_CLOCK);

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

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
