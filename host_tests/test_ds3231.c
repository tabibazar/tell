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
    ds3231_pack(secs, regs);
    int ok = ds3231_unpack(regs, &back) && back == secs % 86400;
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
    ds3231_pack(23 * 3600 + 59 * 60 + 58, regs);
    expect("packed as BCD", regs[0] == 0x58 && regs[1] == 0x59 && regs[2] == 0x23);
    expect("24-hour mode selected", (regs[2] & 0x40) == 0);
    expect("date fields are valid BCD",
           regs[3] >= 1 && regs[3] <= 7 && regs[4] == 0x01 && regs[5] == 0x01);

    uint32_t secs;
    /* 12-hour mode, as a chip set by something else might be in. */
    uint8_t pm[7] = { 0x00, 0x30, 0x40 | 0x20 | 0x01, 1, 1, 1, 0 };
    expect("1:30 PM in 12-hour mode", ds3231_unpack(pm, &secs) && secs == 13 * 3600 + 30 * 60);
    uint8_t am12[7] = { 0x00, 0x00, 0x40 | 0x12, 1, 1, 1, 0 };
    expect("12 AM is midnight", ds3231_unpack(am12, &secs) && secs == 0);
    uint8_t pm12[7] = { 0x00, 0x00, 0x40 | 0x20 | 0x12, 1, 1, 1, 0 };
    expect("12 PM is noon", ds3231_unpack(pm12, &secs) && secs == 12 * 3600);

    uint8_t halted[7] = { 0x80 | 0x05, 0x00, 0x00, 1, 1, 1, 0 };
    expect("DS1307 halt bit is masked", ds3231_unpack(halted, &secs) && secs == 5);

    uint8_t junk[7] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    expect("all ones is not a time", !ds3231_unpack(junk, &secs));
    uint8_t bad_min[7] = { 0x00, 0x60, 0x00, 1, 1, 1, 0 };
    expect("sixty minutes is not a time", !ds3231_unpack(bad_min, &secs));
    uint8_t bad_digit[7] = { 0x0A, 0x00, 0x00, 1, 1, 1, 0 };
    expect("a BCD digit above nine is rejected", !ds3231_unpack(bad_digit, &secs));

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
