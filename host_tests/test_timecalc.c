#include "timecalc.h"

#include <stdio.h>
#include <string.h>

static int failures;

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

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
