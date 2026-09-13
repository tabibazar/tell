#include "aht21.h"
#include "ens160.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

int main(void)
{
    /*
     * ENS160 compensation encoding. Kelvin times sixty-four and per cent
     * times five hundred and twelve, both from ScioSense's own driver. Get
     * either scale wrong and the chip compensates for a room that does not
     * exist -- readings that are plausible, stable and quietly wrong.
     */
    /*
     * Spelled out as literals rather than as the expression the code uses,
     * which would only prove the code agrees with itself. 273.15 K * 64 is
     * 17481.6 and 298.15 * 64 is 19081.6.
     *
     * We round where ScioSense's own driver truncates, so both come out one
     * count higher than their library would give. A count is 1/64 K, or about
     * sixteen thousandths of a degree, which is far below anything the
     * compensation can act on -- but it is a real difference from the
     * reference implementation and worth stating rather than discovering.
     */
    expect("zero Celsius encodes as 17482", ens160_encode_temp(0.0f) == 17482);
    expect("25 C encodes as 19082", ens160_encode_temp(25.0f) == 19082);
    expect("which is never more than one count off truncation",
           ens160_encode_temp(0.0f) - (uint16_t)(273.15f * 64.0f) <= 1
           && ens160_encode_temp(25.0f) - (uint16_t)(298.15f * 64.0f) <= 1);
    /* A degree must move the number by 64, or the scale is wrong. */
    expect("one degree is sixty-four counts",
           ens160_encode_temp(21.0f) - ens160_encode_temp(20.0f) == 64);

    expect("zero humidity is zero", ens160_encode_rh(0.0f) == 0);
    expect("fifty per cent is 25600", ens160_encode_rh(50.0f) == 25600);
    expect("a hundred per cent is 51200", ens160_encode_rh(100.0f) == 51200);

    /* Both encodings are unsigned, so a wild reading must clamp rather than
       wrap round into something that looks reasonable. */
    expect("humidity above the scale clamps", ens160_encode_rh(150.0f) == 51200);
    expect("and below it clamps too", ens160_encode_rh(-20.0f) == 0);
    expect("a temperature below absolute zero does not wrap",
           ens160_encode_temp(-400.0f) == 0);
    expect("and an absurdly hot one saturates instead of wrapping",
           ens160_encode_temp(5000.0f) == 65535);

    /*
     * The validity flag, bits 3:2 of DEVICE_STATUS. This is the field that
     * decides whether a reading is worth storing, so decoding it from the
     * wrong bits would fill a log with confident rubbish.
     */
    expect("normal operation", ens160_validity(0x00) == ENS160_NORMAL);
    expect("warm-up", ens160_validity(0x04) == ENS160_WARMUP);
    expect("initial start-up", ens160_validity(0x08) == ENS160_INITIAL_STARTUP);
    expect("invalid output", ens160_validity(0x0C) == ENS160_INVALID);
    /* The neighbouring bits must not leak into it: NEWDAT and NEWGPR sit
       directly below, and the running and error flags directly above. */
    expect("the data-ready bits do not disturb it",
           ens160_validity(0x03) == ENS160_NORMAL);
    expect("nor do the status bits above it",
           ens160_validity(0xC3) == ENS160_NORMAL);
    expect("and a warm-up is still a warm-up among them",
           ens160_validity(0xC7) == ENS160_WARMUP);

    expect("new data is flagged", ens160_has_new_data(0x02));
    expect("and its absence is too", !ens160_has_new_data(0x01));

    expect("the index reads as words", strcmp(ens160_aqi_name(1), "excellent") == 0
           && strcmp(ens160_aqi_name(5), "unhealthy") == 0);
    expect("and an unknown index does not pretend", strcmp(ens160_aqi_name(0), "--") == 0
           && strcmp(ens160_aqi_name(9), "--") == 0);

    /*
     * AHT21. Twenty bits of each reading, sharing the middle byte a nibble
     * apiece -- humidity takes the high nibble, temperature the low. Taking
     * them the wrong way round gives readings that are wrong and entirely
     * believable, which is why this is checked rather than eyeballed.
     */
    {
        float t, h;

        /* Half scale in both: humidity 50%, temperature 50 C. */
        uint8_t mid[6] = { 0x00, 0x80, 0x00, 0x08, 0x00, 0x00 };
        aht21_convert(mid, &t, &h);
        expect("half scale is fifty per cent", fabsf(h - 50.0f) < 0.01f);
        expect("and fifty degrees", fabsf(t - 50.0f) < 0.01f);

        /* All zeros: 0% and the bottom of the range, which is -50. */
        uint8_t zero[6] = { 0, 0, 0, 0, 0, 0 };
        aht21_convert(zero, &t, &h);
        expect("zero is zero humidity", fabsf(h) < 0.001f);
        expect("and minus fifty degrees", fabsf(t + 50.0f) < 0.001f);

        /* All ones: 100% and the top of the range, 150. */
        uint8_t full[6] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        aht21_convert(full, &t, &h);
        expect("full scale is a hundred per cent", fabsf(h - 100.0f) < 0.02f);
        expect("and a hundred and fifty degrees", fabsf(t - 150.0f) < 0.02f);

        /*
         * The shared byte, proved. Here humidity's bits are all zero and
         * temperature's all one; if the nibbles were swapped both answers
         * would come out the other way round.
         */
        uint8_t split[6] = { 0x00, 0x00, 0x00, 0x0F, 0xFF, 0xFF };
        aht21_convert(split, &t, &h);
        expect("the shared byte's low nibble belongs to temperature",
               fabsf(t - 150.0f) < 0.02f);
        expect("and its high nibble to humidity", fabsf(h) < 0.001f);

        uint8_t swapped[6] = { 0x00, 0x00, 0x00, 0xF0, 0x00, 0x00 };
        aht21_convert(swapped, &t, &h);
        expect("the other way round moves humidity, not temperature",
               h > 0.0f && fabsf(t + 50.0f) < 0.001f);

        /* A real-looking reading: about 24 C and 45%. */
        uint32_t raw_h = (uint32_t)(45.0f / 100.0f * 1048576.0f);
        uint32_t raw_t = (uint32_t)((24.0f + 50.0f) / 200.0f * 1048576.0f);
        uint8_t r[6];
        r[0] = 0x00;
        r[1] = (uint8_t)(raw_h >> 12);
        r[2] = (uint8_t)(raw_h >> 4);
        r[3] = (uint8_t)(((raw_h & 0x0F) << 4) | ((raw_t >> 16) & 0x0F));
        r[4] = (uint8_t)(raw_t >> 8);
        r[5] = (uint8_t)raw_t;
        aht21_convert(r, &t, &h);
        expect("a room reading round-trips", fabsf(t - 24.0f) < 0.01f
               && fabsf(h - 45.0f) < 0.01f);

        /* Humidity is clamped: the formula can overshoot on a faulty part. */
        uint8_t over[6] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        aht21_convert(over, &t, &h);
        expect("humidity never exceeds a hundred", h <= 100.0f);
    }

    /*
     * The CRC. Polynomial 0x31 seeded 0xFF -- the same one Sensirion use, and
     * the reason a garbled reading is discarded rather than charted.
     */
    {
        /* A known vector for this polynomial: 0xBE,0xEF checksums to 0x92. */
        const uint8_t v[2] = { 0xBE, 0xEF };
        expect("the CRC matches the standard vector", aht21_crc(v, 2) == 0x92);

        /* And it must actually detect damage: flipping any single bit of a
           six-byte frame has to change the answer. */
        uint8_t frame[6] = { 0x1C, 0x73, 0x4A, 0x55, 0x2C, 0x61 };
        uint8_t good = aht21_crc(frame, 6);
        int missed = 0;
        for (int byte = 0; byte < 6; byte++)
            for (int bit = 0; bit < 8; bit++) {
                uint8_t bad[6];
                memcpy(bad, frame, sizeof bad);
                bad[byte] ^= (uint8_t)(1u << bit);
                if (aht21_crc(bad, 6) == good) missed++;
            }
        expect("and catches every single-bit error in a frame", missed == 0);
    }

    printf("%s\n", failures ? "FAILURES" : "all tests passed");
    return failures ? 1 : 0;
}
