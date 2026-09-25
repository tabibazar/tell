#include "pcf85063.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

static void roundtrip(const char *what, uint32_t secs)
{
    uint8_t regs[7];
    uint32_t back = 99;
    pcf85063_pack(secs, NULL, regs);
    int ok = pcf85063_unpack(regs, &back, NULL) && back == secs % 86400;
    if (!ok) printf("     %s: %u -> %02X %02X %02X -> %u\n", what, secs,
                    regs[0], regs[1], regs[2], back);
    expect(what, ok);
}

/* A date and time in, the same date and time out, and the registers as the
   datasheet lays them out: day at 07h, weekday at 08h. */
static void date_roundtrip(const char *what, uint32_t secs, ds3231_date_t in)
{
    uint8_t regs[7];
    ds3231_date_t out;
    uint32_t back = 99;
    int packed = pcf85063_pack(secs, &in, regs);
    int ok = packed && pcf85063_unpack(regs, &back, &out)
          && back == secs && ds3231_date_valid(&out)
          && out.year == in.year && out.month == in.month
          && out.day == in.day && out.wday == in.wday;
    if (!ok) printf("     %s: %02X %02X %02X %02X %02X %02X %02X -> %d-%d-%d w%d %u\n",
                    what, regs[0], regs[1], regs[2], regs[3], regs[4], regs[5],
                    regs[6], out.year, out.month, out.day, out.wday, back);
    expect(what, ok);
}

int main(void)
{
    /* The time of day, as ds3231's tests have it. */
    roundtrip("midnight", 0);
    roundtrip("one second", 1);
    roundtrip("last second of the day", 86399);
    roundtrip("afternoon", 13 * 3600 + 45 * 60 + 7);
    roundtrip("noon", 12 * 3600);
    roundtrip("a full day wraps", 86400 + 5);

    uint8_t regs[7];
    uint32_t secs;
    pcf85063_pack(23 * 3600 + 59 * 60 + 59, NULL, regs);
    expect("23:59:59 packed as BCD",
           regs[0] == 0x59 && regs[1] == 0x59 && regs[2] == 0x23);

    /* The dummy date for a NULL is the chip's own power-on value, a real
       Saturday, so it rolls over at midnight like any other day. */
    expect("a NULL date says it was not packed", !pcf85063_pack(0, NULL, regs));
    expect("and packs Saturday 1 January 2000",
           regs[3] == 0x01 && regs[4] == 6 && regs[5] == 0x01 && regs[6] == 0x00);

    /* The calendar. */
    {
        date_roundtrip("a Friday in September",
                       45296, (ds3231_date_t){ 2026, 9, 11, 5 });
        date_roundtrip("the last second the chip can hold",
                       86399, (ds3231_date_t){ 2099, 12, 31, 4 });
        date_roundtrip("a leap day", 12 * 3600,
                       (ds3231_date_t){ 2028, 2, 29, 2 });
        date_roundtrip("the first of the range", 0,
                       (ds3231_date_t){ 2020, 1, 1, 3 });

        ds3231_date_t d = { 2099, 12, 31, 4 };
        pcf85063_pack(86399, &d, regs);
        expect("2099-12-31 23:59:59 as the datasheet lays it out",
               regs[0] == 0x59 && regs[1] == 0x59 && regs[2] == 0x23
               && regs[3] == 0x31 && regs[4] == 4 && regs[5] == 0x12
               && regs[6] == 0x99);
        expect("no century bit in the month", (regs[5] & 0x80) == 0);

        /* Monday to Saturday keep their numbers; Sunday is the chip's 0, so
           Saturday's 6 rolls to Sunday and Sunday's 0 to Monday. */
        ds3231_date_t out;
        ds3231_date_t sun = { 2026, 9, 13, 7 };
        pcf85063_pack(0, &sun, regs);
        expect("Sunday is weekday 0 on the chip", regs[4] == 0);
        expect("and 7 again when read back",
               pcf85063_unpack(regs, &secs, &out) && out.wday == 7);
        ds3231_date_t mon = { 2026, 9, 14, 1 };
        pcf85063_pack(0, &mon, regs);
        expect("Monday is weekday 1", regs[4] == 1);

        /* 29 February only in a leap year. The chip would not hold the other
           kind, so it is neither written nor believed. */
        ds3231_date_t feb29 = { 2026, 2, 29, 7 };
        expect("29 Feb 2026 is not packed", !pcf85063_pack(0, &feb29, regs));
        uint8_t feb29_regs[7] = { 0x00, 0x00, 0x12, 0x29, 0x00, 0x02, 0x26 };
        pcf85063_unpack(feb29_regs, &secs, &out);
        expect("nor believed when read", !ds3231_date_valid(&out));
        ds3231_date_t apr31 = { 2026, 4, 31, 5 };
        expect("nor is 31 April", !pcf85063_pack(0, &apr31, regs));
        ds3231_date_t bad = { 2026, 13, 1, 1 };
        expect("nor a month that does not exist", !pcf85063_pack(0, &bad, regs));
    }

    /* The oscillator-stop flag, bit 7 of the seconds. */
    {
        ds3231_date_t d = { 2026, 9, 24, 4 };
        pcf85063_pack(59, &d, regs);
        expect("packing leaves OS clear, so a write clears it",
               (regs[0] & 0x80) == 0);
        expect("and cleared registers are trusted",
               pcf85063_untrusted(0x00, regs) == NULL);

        regs[0] |= 0x80;
        expect("OS set is not trusted", pcf85063_untrusted(0x00, regs) != NULL);
        expect("but still decodes, masked", pcf85063_unpack(regs, &secs, NULL) && secs == 59);

        /* What a chip reads just after power-up: OS set, 00:00:00, and its
           reset date of Saturday 1 January 2000. */
        uint8_t fresh[7] = { 0x80, 0x00, 0x00, 0x01, 0x06, 0x01, 0x00 };
        ds3231_date_t out;
        expect("a fresh chip is not trusted", pcf85063_untrusted(0x00, fresh) != NULL);
        expect("and its date is not believed either",
               pcf85063_unpack(fresh, &secs, &out) && secs == 0
               && !ds3231_date_valid(&out) && out.year == 0);

        /* Control_1 can make good registers untrustworthy too. */
        pcf85063_pack(59, &d, regs);
        expect("STOP set is not trusted", pcf85063_untrusted(0x20, regs) != NULL);
        expect("12-hour mode is not trusted", pcf85063_untrusted(0x02, regs) != NULL);
        expect("external test mode is not trusted", pcf85063_untrusted(0x80, regs) != NULL);
        expect("CAP_SEL and CIE are harmless",
               pcf85063_untrusted(0x01 | 0x04, regs) == NULL);
    }

    /* Registers that are not a time. */
    {
        uint8_t junk[7] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        expect("all ones is not a time", !pcf85063_unpack(junk, &secs, NULL));
        uint8_t bad_sec[7] = { 0x0A, 0x00, 0x00, 0x01, 0x01, 0x01, 0x26 };
        expect("a seconds digit above nine is rejected", !pcf85063_unpack(bad_sec, &secs, NULL));
        uint8_t sixty_s[7] = { 0x60, 0x00, 0x00, 0x01, 0x01, 0x01, 0x26 };
        expect("sixty seconds is not a time", !pcf85063_unpack(sixty_s, &secs, NULL));
        uint8_t bad_min[7] = { 0x00, 0x60, 0x00, 0x01, 0x01, 0x01, 0x26 };
        expect("sixty minutes is not a time", !pcf85063_unpack(bad_min, &secs, NULL));
        uint8_t min_digit[7] = { 0x00, 0x3F, 0x00, 0x01, 0x01, 0x01, 0x26 };
        expect("a minutes digit above nine is rejected", !pcf85063_unpack(min_digit, &secs, NULL));
        uint8_t h24[7] = { 0x00, 0x00, 0x24, 0x01, 0x01, 0x01, 0x26 };
        expect("24 o'clock is not a time", !pcf85063_unpack(h24, &secs, NULL));
        uint8_t h_digit[7] = { 0x00, 0x00, 0x1A, 0x01, 0x01, 0x01, 0x26 };
        expect("an hours digit above nine is rejected", !pcf85063_unpack(h_digit, &secs, NULL));
        uint8_t spare[7] = { 0x00, 0x80 | 0x30, 0xC0 | 0x13, 0x01, 0x01, 0x01, 0x26 };
        expect("unused bits in minutes and hours are masked",
               pcf85063_unpack(spare, &secs, NULL) && secs == 13 * 3600 + 30 * 60);

        /* A good time with a bad date keeps the time and drops the date,
           as ds3231_unpack does. */
        ds3231_date_t out;
        uint8_t day_digit[7] = { 0x00, 0x00, 0x12, 0x1A, 0x01, 0x09, 0x26 };
        expect("a day digit above nine keeps the time but not the date",
               pcf85063_unpack(day_digit, &secs, &out) && secs == 12 * 3600
               && !ds3231_date_valid(&out));
        uint8_t month_digit[7] = { 0x00, 0x00, 0x12, 0x01, 0x01, 0x0A, 0x26 };
        pcf85063_unpack(month_digit, &secs, &out);
        expect("a month digit above nine is not a date", !ds3231_date_valid(&out));
        uint8_t year_digit[7] = { 0x00, 0x00, 0x12, 0x01, 0x01, 0x09, 0x2A };
        pcf85063_unpack(year_digit, &secs, &out);
        expect("a year digit above nine is not a date", !ds3231_date_valid(&out));
        uint8_t wday7[7] = { 0x00, 0x00, 0x12, 0x01, 0x07, 0x09, 0x26 };
        pcf85063_unpack(wday7, &secs, &out);
        expect("weekday 7 does not exist on the chip", !ds3231_date_valid(&out));
        uint8_t day0[7] = { 0x00, 0x00, 0x12, 0x00, 0x01, 0x09, 0x26 };
        pcf85063_unpack(day0, &secs, &out);
        expect("nor does day 0", !ds3231_date_valid(&out));

        /* And it formats as the DS3231's does, since main.c shares that. */
        uint8_t good[7] = { 0x00, 0x00, 0x12, 0x24, 0x04, 0x09, 0x26 };
        char buf[32];
        pcf85063_unpack(good, &secs, &out);
        ds3231_format_date(&out, buf, sizeof buf);
        expect("24 September 2026 reads as a Thursday",
               strcmp(buf, "Thu 24 Sep 2026") == 0);
    }

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
