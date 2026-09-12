#include "bme280.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

/*
 * A plausible calibration set, of the shape a real chip returns. The absolute
 * numbers are not the point -- without a chip on the bench there is nothing
 * to check them against -- so these tests are about the things that go wrong
 * in this driver regardless of the constants: the bit packing, the order the
 * three readings must be taken in, and the ends of the ranges.
 */
static void sample_calib(bme280_cal_t *c)
{
    uint8_t tp[26] = {
        0x70, 0x6B,             /* T1 = 27504 */
        0x43, 0x67,             /* T2 = 26435 */
        0x18, 0xFC,             /* T3 = -1000 */
        0x7D, 0x8E,             /* P1 = 36477 */
        0x43, 0xD6,             /* P2 = -10687 */
        0xD0, 0x0B,             /* P3 = 3024 */
        0x27, 0x0B,             /* P4 = 2855 */
        0x8C, 0x00,             /* P5 = 140 */
        0xF9, 0xFF,             /* P6 = -7 */
        0x8C, 0x3C,             /* P7 = 15500 */
        0xF8, 0xC6,             /* P8 = -14600 */
        0x70, 0x17,             /* P9 = 6000 */
        0x00,                   /* reserved */
        0x4B,                   /* H1 = 75 */
    };
    /* 0xE1..0xE7. H4 and H5 share 0xE5: its low nibble finishes H4, its high
       nibble starts H5. */
    uint8_t h[7] = { 0x62, 0x01, 0x00, 0x13, 0x48, 0x03, 0x1E };
    bme280_parse_calib(tp, h, c);
}

int main(void)
{
    bme280_cal_t c;
    sample_calib(&c);

    expect("T1 is unsigned", c.t1 == 27504);
    expect("T2 is signed", c.t2 == 26435);
    expect("T3 is negative", c.t3 == -1000);
    expect("P6 is a small negative", c.p6 == -7);
    expect("H1 comes from the end of the first block", c.h1 == 75);
    expect("H2 is little-endian signed", c.h2 == 354);

    /*
     * The nibble packing, which is the single easiest thing to get wrong
     * here: 0xE4 = 0x13 and 0xE5 = 0x48, so H4 is 0x138 and H5 takes 0xE5's
     * high nibble, making 0x034. Getting these the wrong way round leaves
     * humidity plausible but wrong, which is worse than broken.
     */
    expect("H4 takes the low nibble of the shared byte", c.h4 == 0x138);
    expect("H5 takes the high one", c.h5 == 0x034);
    expect("H6 is signed", c.h6 == 30);

    /* Twelve-bit values are signed: a high bit set means negative. */
    {
        bme280_cal_t n;
        uint8_t tp[26] = {0};
        uint8_t h[7] = { 0, 0, 0, 0xFF, 0xFF, 0xFF, 0 };
        bme280_parse_calib(tp, h, &n);
        expect("a twelve-bit calibration value can be negative",
               n.h4 == -1 && n.h5 == -1);
    }

    /* The raw unpacking: twenty bits for pressure and temperature, sixteen
       for humidity, all big-endian in the chip's data block. */
    {
        uint8_t d[8] = { 0x51, 0x7C, 0x00, 0x80, 0x4E, 0x00, 0x64, 0x1C };
        int32_t p, t, h;
        bme280_raw(d, &p, &t, &h);
        expect("pressure is twenty bits", p == 0x517C0);
        expect("temperature is twenty bits", t == 0x804E0);
        expect("humidity is sixteen", h == 0x641C);
    }

    /* Temperature must be compensated first, because it sets the t_fine the
       other two read. A pressure taken before it is not merely inaccurate,
       it is computed from a stale or zero value. */
    {
        bme280_cal_t fresh;
        sample_calib(&fresh);
        expect("t_fine starts at nothing", fresh.t_fine == 0);
        float t = bme280_temperature(&fresh, 0x80000);
        expect("and the temperature sets it", fresh.t_fine != 0);
        expect("which is a believable room", t > -50.0f && t < 80.0f);
    }

    /* Warmer readings must read warmer: a sign slip in the second term would
       not show up in any single value. */
    {
        bme280_cal_t a, b;
        sample_calib(&a); sample_calib(&b);
        float cool = bme280_temperature(&a, 0x7F000);
        float warm = bme280_temperature(&b, 0x81000);
        expect("a higher reading is a higher temperature", warm > cool);
    }

    /* Pressure: a believable sea-level sort of number, and no divide by zero
       on a chip that returned nonsense. */
    {
        bme280_temperature(&c, 0x80000);
        float hpa = bme280_pressure(&c, 0x50000);
        expect("pressure is in hectopascals, not pascals",
               hpa > 300.0f && hpa < 1200.0f);

        bme280_cal_t zero;
        memset(&zero, 0, sizeof zero);
        expect("a zero calibration gives zero, not a crash",
               bme280_pressure(&zero, 0x50000) == 0.0f);
    }

    /* Humidity is clamped: the formula overshoots at both ends rather than
       saturating, so a damp room could otherwise read over a hundred. */
    {
        bme280_temperature(&c, 0x80000);
        float h = bme280_humidity(&c, 0x6000);
        expect("humidity is a percentage", h >= 0.0f && h <= 100.0f);
        expect("an absurdly low reading clamps at zero",
               bme280_humidity(&c, -1000000) == 0.0f);
        expect("an absurdly high one clamps at a hundred",
               bme280_humidity(&c, 1000000) == 100.0f);
    }

    printf("%s\n", failures ? "FAILURES" : "all tests passed");
    return failures ? 1 : 0;
}
