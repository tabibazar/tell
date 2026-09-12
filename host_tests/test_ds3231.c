#include "ds3231.h"

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
    ds3231_pack(secs, NULL, regs);
    int ok = ds3231_unpack(regs, &back, NULL) && back == secs % 86400;
    if (!ok) printf("     %s: %u -> %02X %02X %02X -> %u\n", what, secs,
                    regs[0], regs[1], regs[2], back);
    expect(what, ok);
}

int main(void)
{
    roundtrip("midnight", 0);
    roundtrip("one second", 1);
    roundtrip("last second of the day", 86399);
    roundtrip("afternoon", 13 * 3600 + 45 * 60 + 7);
    roundtrip("noon", 12 * 3600);
    roundtrip("a full day wraps", 86400 + 5);

    uint8_t regs[7];
    ds3231_pack(23 * 3600 + 59 * 60 + 58, NULL, regs);
    expect("packed as BCD", regs[0] == 0x58 && regs[1] == 0x59 && regs[2] == 0x23);
    expect("24-hour mode selected", (regs[2] & 0x40) == 0);
    expect("date fields are valid BCD",
           regs[3] >= 1 && regs[3] <= 7 && regs[4] == 0x01 && regs[5] == 0x01);

    uint32_t secs;
    /* 12-hour mode, as a chip set by something else might be in. */
    uint8_t pm[7] = { 0x00, 0x30, 0x40 | 0x20 | 0x01, 1, 1, 1, 0 };
    expect("1:30 PM in 12-hour mode", ds3231_unpack(pm, &secs, NULL) && secs == 13 * 3600 + 30 * 60);
    uint8_t am12[7] = { 0x00, 0x00, 0x40 | 0x12, 1, 1, 1, 0 };
    expect("12 AM is midnight", ds3231_unpack(am12, &secs, NULL) && secs == 0);
    uint8_t pm12[7] = { 0x00, 0x00, 0x40 | 0x20 | 0x12, 1, 1, 1, 0 };
    expect("12 PM is noon", ds3231_unpack(pm12, &secs, NULL) && secs == 12 * 3600);

    uint8_t halted[7] = { 0x80 | 0x05, 0x00, 0x00, 1, 1, 1, 0 };
    expect("DS1307 halt bit is masked", ds3231_unpack(halted, &secs, NULL) && secs == 5);

    uint8_t junk[7] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    expect("all ones is not a time", !ds3231_unpack(junk, &secs, NULL));
    uint8_t bad_min[7] = { 0x00, 0x60, 0x00, 1, 1, 1, 0 };
    expect("sixty minutes is not a time", !ds3231_unpack(bad_min, &secs, NULL));
    uint8_t bad_digit[7] = { 0x0A, 0x00, 0x00, 1, 1, 1, 0 };
    expect("a BCD digit above nine is rejected", !ds3231_unpack(bad_digit, &secs, NULL));

    /* The calendar. A board that has been unplugged knows the time from the
       chip and would otherwise have no idea what day it is, so the chip keeps
       the date too and rolls it over on its own battery. */
    {
        ds3231_date_t in = { 2026, 9, 11, 5 };   /* Friday 11 September 2026 */
        ds3231_date_t out;
        uint32_t back = 0;
        uint8_t dregs[7];
        ds3231_pack(45296, &in, dregs);
        expect("the century bit marks 20xx", (dregs[5] & 0x80) != 0);
        expect("a date survives the registers",
               ds3231_unpack(dregs, &back, &out)
               && out.year == 2026 && out.month == 9 && out.day == 11
               && out.wday == 5);
        expect("and the time with it", back == 45296);

        char buf[32];
        ds3231_format_date(&out, buf, sizeof buf);
        expect("it reads as a date", strcmp(buf, "Fri 11 Sep 2026") == 0);

        /* A chip nobody has set reads as 2000, which is not a date to show. */
        ds3231_pack(0, NULL, dregs);
        ds3231_unpack(dregs, &back, &out);
        expect("a dummy date is not believed", !ds3231_date_valid(&out));
        ds3231_format_date(&out, buf, sizeof buf);
        expect("and formats as nothing at all", buf[0] == '\0');

        ds3231_date_t bad = { 2026, 13, 1, 1 };
        expect("nor is a month that does not exist", !ds3231_date_valid(&bad));
        expect("nor a NULL", !ds3231_date_valid(NULL));
    }

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
