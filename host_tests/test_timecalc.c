#include "timecalc.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

static void expect_fmt(const char *what, uint32_t secs, const char *want)
{
    char got[6];
    timecalc_format(secs, got);
    if (strcmp(got, want) == 0) { printf("ok   %s\n", what); return; }
    printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
    failures++;
}

static void expect_civil(const char *what, int32_t days, int y, int m, int d,
                         int wd)
{
    int gy, gm, gd;
    timecalc_civil(days, &gy, &gm, &gd);
    int gw = timecalc_weekday(days);
    if (gy == y && gm == m && gd == d && gw == wd) {
        printf("ok   %s\n", what);
        return;
    }
    printf("FAIL %s: got %04d-%02d-%02d wd %d, want %04d-%02d-%02d wd %d\n",
           what, gy, gm, gd, gw, y, m, d, wd);
    failures++;
}

static void expect_adv(const char *what, uint32_t base, uint64_t us, uint32_t want)
{
    uint32_t got = timecalc_advance(base, us);
    if (got == want) { printf("ok   %s\n", what); return; }
    printf("FAIL %s: got %u, want %u\n", what, got, want);
    failures++;
}

static void expect_hms(const char *what, uint32_t secs, const char *want)
{
    char got[9];
    timecalc_format_hms(secs, got);
    if (strcmp(got, want) == 0) { printf("ok   %s\n", what); return; }
    printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
    failures++;
}

int main(void)
{
    expect_fmt("midnight", 0, "00:00");
    expect_fmt("one minute", 60, "00:01");
    expect_fmt("noon", 12 * 3600, "12:00");
    expect_fmt("last minute of day", 23 * 3600 + 59 * 60, "23:59");
    expect_fmt("seconds are dropped", 12 * 3600 + 34 * 60 + 56, "12:34");
    expect_fmt("wraps a full day", SECS_PER_DAY + 3600, "01:00");

    expect_hms("hms midnight", 0, "00:00:00");
    expect_hms("hms keeps seconds", 12 * 3600 + 34 * 60 + 56, "12:34:56");
    expect_hms("hms last second of day", SECS_PER_DAY - 1, "23:59:59");
    expect_hms("hms wraps a full day", SECS_PER_DAY + 61, "00:01:01");

    expect_adv("no elapsed time", 100, 0, 100);
    expect_adv("sub-second ignored", 100, 999999ull, 100);
    expect_adv("one second", 100, 1000000ull, 101);
    expect_adv("crosses midnight",
               23 * 3600 + 59 * 60 + 59, 2000000ull, 1);
    expect_adv("multiple days wrap",
               0, (uint64_t)SECS_PER_DAY * 3 * 1000000ull + 5000000ull, 5);

    /* Day counts from Python: date.toordinal() - date(1970,1,1).toordinal();
       weekdays 0 Sunday .. 6 Saturday. */
    expect_civil("epoch is a Thursday", 0, 1970, 1, 1, 4);
    expect_civil("day before the epoch", -1, 1969, 12, 31, 3);
    expect_civil("leap day 2024", 19782, 2024, 2, 29, 4);
    expect_civil("day after leap day", 19783, 2024, 3, 1, 5);
    expect_civil("end of 1999", 10956, 1999, 12, 31, 5);
    expect_civil("start of 2000", 10957, 2000, 1, 1, 6);
    expect_civil("new year 2026", 20454, 2026, 1, 1, 4);
    expect_civil("the day this was written", 20703, 2026, 9, 7, 1);

    if (strcmp(timecalc_month_abbr(1), "Jan") != 0
        || strcmp(timecalc_month_abbr(12), "Dec") != 0
        || strcmp(timecalc_month_abbr(0), "???") != 0) {
        printf("FAIL month abbreviations\n");
        failures++;
    } else printf("ok   month abbreviations\n");

    /*
     * Date to days, and back. The round trip is the assertion worth making:
     * calendar arithmetic is where off-by-one errors hide for years, and a
     * log stamped with the wrong day is wrong for ever after. Every day from
     * 1970 to 2050 goes out and comes back.
     */
    {
        int bad = 0, checked = 0;
        for (int32_t days = 0; days < 29220; days++) {       /* 1970..2050 */
            int y, m, d;
            timecalc_civil(days, &y, &m, &d);
            if (timecalc_days(y, m, d) != days) bad++;
            checked++;
        }
        expect("every day from 1970 to 2050 round-trips", bad == 0 && checked == 29220);

        /* And the anchors, spelled out, so a round trip that is consistently
           wrong by a day would still be caught. */
        expect("the epoch is day zero", timecalc_days(1970, 1, 1) == 0);
        expect("a leap day lands right", timecalc_days(2024, 2, 29) == 19782);
        expect("the day after a leap day too", timecalc_days(2024, 3, 1) == 19783);
        expect("a century non-leap year is handled",
               timecalc_days(1900, 3, 1) - timecalc_days(1900, 2, 28) == 1);
        expect("and a four-hundred-year leap year is",
               timecalc_days(2000, 3, 1) - timecalc_days(2000, 2, 28) == 2);
    }

    /*
     * The clamp that stops a re-based clock reading as an arbitrary time.
     *
     * This is a regression test with a body count: without it, one reading an
     * hour went into wave's environment log carrying a timestamp sixteen
     * hours out, because the hourly RTC re-sync moves the base forward while
     * the caller is still holding a `now` captured at the top of the loop.
     */
    {
        const uint32_t base = 12 * 3600;               /* midday */
        const int64_t  base_us = 5000000;

        expect("an hour on reads an hour on",
               timecalc_since(base, base_us, base_us + 3600 * 1000000LL)
               == base + 3600);
        expect("the same instant reads as the base",
               timecalc_since(base, base_us, base_us) == base);

        /*
         * The case that bit: `now` is a few milliseconds OLDER than the base.
         * Unsigned subtraction makes that about 1.8e19 microseconds, which is
         * six hundred thousand years, and taken modulo a day it lands
         * somewhere entirely plausible and entirely wrong.
         */
        expect("a reading older than the base does not wrap round",
               timecalc_since(base, base_us, base_us - 3000) == base);
        expect("nor does one a whole second older",
               timecalc_since(base, base_us, base_us - 1000000) == base);
        expect("nor an absurdly old one",
               timecalc_since(base, base_us, 0) == base);

        /* And prove the failure it prevents is the one that happened: fed the
           same backwards gap, the unguarded arithmetic gives neither the base
           nor anything near it. */
        {
            uint32_t unguarded = timecalc_advance(base, (uint64_t)(-3000));
            expect("the unguarded form really does produce a wrong time of day",
                   unguarded != base);
        }

        /* Midnight still wraps normally through the clamp. */
        expect("a day later is the same time",
               timecalc_since(base, base_us, base_us + 86400 * 1000000LL) == base);
    }

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
