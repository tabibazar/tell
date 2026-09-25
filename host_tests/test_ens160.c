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

/* A seven-byte AHT21 frame for a given status, temperature and humidity,
   with the CRC it should carry. */
static void make_frame(uint8_t f[7], uint8_t status, float celsius, float humidity)
{
    uint32_t raw_h = (uint32_t)(humidity / 100.0f * 1048576.0f);
    uint32_t raw_t = (uint32_t)((celsius + 50.0f) / 200.0f * 1048576.0f);
    f[0] = status;
    f[1] = (uint8_t)(raw_h >> 12);
    f[2] = (uint8_t)(raw_h >> 4);
    f[3] = (uint8_t)(((raw_h & 0x0F) << 4) | ((raw_t >> 16) & 0x0F));
    f[4] = (uint8_t)(raw_t >> 8);
    f[5] = (uint8_t)raw_t;
    f[6] = aht21_crc(f, 6);
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

    /*
     * What the gas sensor is told. The bench case this exists for: envo's
     * AHT21 failing every CRC, its BMP280 about to come out, and the gas
     * readings -- the point of the board -- compensated with whatever is left.
     */
    {
        const int64_t MIN = 60LL * 1000000;
        ens160_air_t a;
        ens160_comp_t c;

        /* Everything working: the AHT21 wins both, the barometer is ignored. */
        memset(&a, 0, sizeof a);
        a.aht_ok = true; a.aht_c = 22.5f; a.aht_rh = 68.0f;
        a.bmx_ok = true; a.bmx_c = 24.0f;
        c = ens160_choose_comp(&a);
        expect("a good AHT21 supplies the temperature",
               c.celsius == 22.5f && c.t_from == ENS160_FROM_AHT21);
        expect("and the humidity",
               c.humidity == 68.0f && c.rh_from == ENS160_FROM_AHT21 && c.rh_age_us == 0);

        /* AHT21 failing its CRC, barometer answering, a good humidity five
           minutes old: the barometer's temperature and the held humidity. */
        memset(&a, 0, sizeof a);
        a.bmx_ok = true; a.bmx_c = 24.0f;
        a.held_ok = true; a.held_rh = 70.0f; a.held_age_us = 5 * MIN;
        c = ens160_choose_comp(&a);
        expect("with the AHT21 failing, the barometer supplies the temperature",
               c.celsius == 24.0f && c.t_from == ENS160_FROM_BMX280);
        expect("and a recent good humidity is held",
               c.humidity == 70.0f && c.rh_from == ENS160_FROM_AHT21_HELD
               && c.rh_age_us == 5 * MIN);

        /* The same, but the held humidity is eleven minutes old. */
        a.held_age_us = 11 * MIN;
        c = ens160_choose_comp(&a);
        expect("a stale humidity gives way to the chip's own 50 %",
               c.humidity == ENS160_DEFAULT_RH
               && c.rh_from == ENS160_FROM_DEFAULT);
        a.held_age_us = 10 * MIN;
        c = ens160_choose_comp(&a);
        expect("and ten minutes exactly is already too old",
               c.rh_from == ENS160_FROM_DEFAULT);
        a.held_age_us = 10 * MIN - 1;
        c = ens160_choose_comp(&a);
        expect("where a moment under is still held", c.rh_from == ENS160_FROM_AHT21_HELD);

        /* A BMP280 has no hygrometer and reports zero; it must never be
           mistaken for a desert. */
        memset(&a, 0, sizeof a);
        a.bmx_ok = true; a.bmx_c = 24.0f;
        a.bmx_rh_ok = false; a.bmx_rh = 0.0f;
        c = ens160_choose_comp(&a);
        expect("a BMP280's missing humidity is not written as zero",
               c.humidity == ENS160_DEFAULT_RH);
        /* Even flagged as real, zero is refused. */
        a.bmx_rh_ok = true;
        c = ens160_choose_comp(&a);
        expect("nor is a zero from anywhere else", c.humidity == ENS160_DEFAULT_RH);
        a.bmx_rh = 55.0f;
        c = ens160_choose_comp(&a);
        expect("while a BME280's real humidity is used",
               c.humidity == 55.0f && c.rh_from == ENS160_FROM_BMX280);
        /* ...but a recent AHT21 one, from beside the gas sensor, comes first. */
        a.held_ok = true; a.held_rh = 70.0f; a.held_age_us = 1 * MIN;
        c = ens160_choose_comp(&a);
        expect("behind a held AHT21 humidity", c.rh_from == ENS160_FROM_AHT21_HELD);

        /* An AHT21 whose humidity is zero still has a usable temperature. */
        memset(&a, 0, sizeof a);
        a.aht_ok = true; a.aht_c = 21.0f; a.aht_rh = 0.0f;
        c = ens160_choose_comp(&a);
        expect("an AHT21 reading zero humidity keeps its temperature",
               c.t_from == ENS160_FROM_AHT21 && c.celsius == 21.0f);
        expect("but not its zero", c.humidity == ENS160_DEFAULT_RH);

        /*
         * The barometer pulled and the AHT21 failing. The chip is never left
         * to keep whatever it was last told: the old code wrote nothing here,
         * so a value written once -- by a chance CRC pass, say -- stayed in
         * force for as long as the thermometer was silent. Instead a recent
         * real temperature is held, as a humidity is, and past that the chip
         * gets its own power-on default, written, so the log and the chip
         * agree on what it is using.
         */
        memset(&a, 0, sizeof a);
        a.held_ok = true; a.held_rh = 70.0f; a.held_age_us = 3 * MIN;
        a.held_t_ok = true; a.held_c = 22.0f; a.held_t_age_us = 3 * MIN;
        c = ens160_choose_comp(&a);
        expect("with no thermometer now, a recent real temperature is held",
               c.celsius == 22.0f && c.t_from == ENS160_FROM_HELD
               && c.t_age_us == 3 * MIN);
        expect("beside the held humidity", c.rh_from == ENS160_FROM_AHT21_HELD);

        a.held_t_age_us = a.held_age_us = 10 * MIN;
        c = ens160_choose_comp(&a);
        expect("ten minutes on, the chip's own default temperature is written",
               c.celsius == ENS160_DEFAULT_C && c.t_from == ENS160_FROM_DEFAULT
               && c.t_age_us == 0);
        expect("with its default humidity",
               c.humidity == ENS160_DEFAULT_RH && c.rh_from == ENS160_FROM_DEFAULT);

        memset(&a, 0, sizeof a);
        c = ens160_choose_comp(&a);
        expect("with nothing ever measured, the default too",
               c.t_from == ENS160_FROM_DEFAULT && c.rh_from == ENS160_FROM_DEFAULT);
        /* And that default is the chip's own: DATA_T and DATA_RH read 0x4A8A
           and 0x6400 until TEMP_IN and RH_IN are written (ENS160 datasheet
           v1.3, 16.2.12 and 16.2.13), so writing it changes nothing but what
           the log can say. */
        expect("which encodes to the chip's power-on 0x4A8A and 0x6400",
               ens160_encode_temp(c.celsius) == 0x4A8A
               && ens160_encode_rh(c.humidity) == 0x6400);

        /* A held temperature is only held; one measured now comes first. */
        a.held_t_ok = true; a.held_c = 30.0f; a.held_t_age_us = MIN;
        a.bmx_ok = true; a.bmx_c = 24.0f;
        c = ens160_choose_comp(&a);
        expect("a barometer now beats a held temperature",
               c.t_from == ENS160_FROM_BMX280 && c.celsius == 24.0f);

        /* A NaN is not a temperature. */
        memset(&a, 0, sizeof a);
        a.aht_ok = true; a.aht_c = NAN; a.aht_rh = 50.0f;
        a.bmx_ok = true; a.bmx_c = 23.0f;
        c = ens160_choose_comp(&a);
        expect("a NaN temperature falls through to the barometer",
               c.t_from == ENS160_FROM_BMX280 && c.celsius == 23.0f);

        /* Whatever the inputs, a written humidity is never zero. */
        int zeros = 0;
        const float rhs[4] = { 0.0f, -5.0f, 0.0f, 150.0f };
        for (int i = 0; i < 64; i++) {
            memset(&a, 0, sizeof a);
            a.aht_ok = i & 1;  a.aht_c = 20.0f; a.aht_rh = rhs[(i >> 1) & 3];
            a.bmx_ok = (i >> 3) & 1; a.bmx_c = 20.0f;
            a.bmx_rh_ok = (i >> 4) & 1; a.bmx_rh = 0.0f;
            a.held_ok = (i >> 5) & 1; a.held_rh = 0.0f; a.held_age_us = MIN;
            c = ens160_choose_comp(&a);
            if (!(c.humidity > 0.0f)) zeros++;
        }
        expect("no combination of inputs writes a humidity of zero", zeros == 0);

        expect("the sources read as words",
               strcmp(ens160_source_name(ENS160_FROM_AHT21), "AHT21") == 0
               && strcmp(ens160_source_name(ENS160_FROM_DEFAULT), "default") == 0
               && strcmp(ens160_source_name(ENS160_FROM_HELD), "held") == 0
               && strcmp(ens160_source_name(ENS160_FROM_NONE), "--") == 0);
    }

    /*
     * Two readers in one pass. envo's flash sampler and its SD log both ask
     * for the gas, about a hundred milliseconds apart when a five-minute
     * sample falls in a thirty-second slot -- which in field sleep is every
     * five minutes. The chip clears NEWDAT at the first DATA read and sets it
     * again a second later, so the second reader used to find nothing: its
     * CSV line lost the gas, or, with no thermometer, was never written.
     * This walks that pass against a model of the chip's NEWDAT.
     */
    {
        const int64_t MS = 1000;
        ens160_gas_t last;
        memset(&last, 0, sizeof last);
        expect("nothing is recent before the first reading",
               !ens160_gas_recent(&last, 0));

        /* The chip: a new reading each second, NEWDAT cleared when read. */
        bool newdat = true;
        int64_t t0 = 300000 * MS;
        int got = 0;
        for (int reader = 0; reader < 2; reader++) {
            int64_t now = t0 + reader * 100 * MS;
            if (newdat) {
                newdat = false;
                ens160_gas_keep(&last, now, 512, 85, 2, ENS160_NORMAL);
            }
            if (ens160_gas_recent(&last, now)) got++;
        }
        expect("both readers in one pass get the gas", got == 2);
        expect("and the same reading",
               last.eco2_ppm == 512 && last.tvoc_ppb == 85 && last.aqi == 2
               && last.validity == ENS160_NORMAL && last.at_us == t0);

        expect("a reading under two seconds old is still the newest there is",
               ens160_gas_recent(&last, t0 + 1999 * MS));
        expect("but at two seconds the chip has stopped, and it is not reused",
               !ens160_gas_recent(&last, t0 + 2000 * MS));
        expect("nor is one stamped in the future",
               !ens160_gas_recent(&last, t0 - 1));
    }

    /*
     * The AHT21's CRC tally: one line per ten minutes, with the rate and the
     * newest bad frame, instead of a warning per read.
     */
    {
        const int64_t MIN = 60LL * 1000000;
        aht21_tally_t t;
        memset(&t, 0, sizeof t);
        char line[160];

        uint8_t good[7] = { 0x1C, 0x73, 0x4A, 0x55, 0x2C, 0x61, 0 };
        good[6] = aht21_crc(good, 6);
        uint8_t bad[7] = { 0x1C, 0x10, 0x20, 0x35, 0x2C, 0x61, 0x00 };
        uint8_t worse[7] = { 0x98, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAB };

        /* Twenty reads, thirty seconds apart, every one bad. */
        int64_t now = 1000;
        for (int i = 0; i < 20; i++, now += MIN / 2)
            aht21_tally_note(&t, now, i == 19 ? worse : bad, false);
        expect("nothing is said before ten minutes have passed",
               !aht21_tally_report(&t, now - MIN / 2, line, sizeof line));
        expect("and at ten minutes the tally is reported",
               aht21_tally_report(&t, 1000 + 10 * MIN, line, sizeof line));
        expect("as a count of the reads",
               strstr(line, "20 of 20 reads failed CRC in the last 10 min") != NULL);
        char want[8];
        snprintf(want, sizeof want, "want %02x", aht21_crc(worse, 6));
        expect("with the newest bad frame's bytes",
               strstr(line, "98 00 00 00 00 00 crc ab") != NULL
               && strstr(line, want) != NULL);

        /* The next window starts from the report, empty. */
        expect("and the count starts again", t.reads == 0 && t.bad == 0);
        expect("so nothing more is said straight away",
               !aht21_tally_report(&t, 1000 + 11 * MIN, line, sizeof line));

        /* A mixed window: only the failures are failures. */
        now = 1000 + 10 * MIN;
        for (int i = 0; i < 10; i++, now += MIN)
            aht21_tally_note(&t, now, (i % 5 == 0) ? bad : good, i % 5 != 0);
        expect("a partly failing sensor is reported by its rate",
               aht21_tally_report(&t, 1000 + 20 * MIN, line, sizeof line)
               && strstr(line, "2 of 10 reads failed CRC") != NULL);

        /* A clean window says nothing, and still starts the next. */
        now = 1000 + 20 * MIN;
        for (int i = 0; i < 20; i++, now += MIN / 2)
            aht21_tally_note(&t, now, good, true);
        line[0] = '\0';
        expect("a window with no failures stays quiet",
               !aht21_tally_report(&t, 1000 + 30 * MIN, line, sizeof line)
               && line[0] == '\0');
        expect("and resets its count all the same", t.reads == 0);
    }

    /*
     * Which CRC-good frames deserve belief. A CRC of eight bits passes about
     * one damaged frame in 256 by chance -- 1 in 259 when bits were dropped
     * at random from a real 23 C / 70 % frame -- so on envo's bus, where every
     * frame was damaged, the only frames the CRC let through would be
     * garbage. These are the bench's own bad frames with their CRC byte
     * repaired: exactly what a chance pass looks like.
     */
    {
        aht21_vet_t v;
        float c = -99.0f, rh = -99.0f;

        /* The bench frames, each given the CRC it wanted. */
        uint8_t bench[4][7] = {
            { 0x18, 0x30, 0x01, 0xd4, 0x00, 0x00, 0 },    /* 0.00 C, 18.8 % */
            { 0x18, 0x0c, 0x80, 0x34, 0x32, 0x8c, 0 },    /* 2.47 C,  4.9 % */
            { 0x18, 0x20, 0x01, 0x04, 0x00, 0x80, 0 },    /* 0.02 C, 12.5 % */
            { 0x18, 0x34, 0x00, 0x55, 0xff, 0x00, 0 },    /* 24.95 C, 20.3 % */
        };
        for (int i = 0; i < 4; i++) bench[i][6] = aht21_crc(bench[i], 6);

        uint8_t room[7], room2[7], breath[7], busy[7], nocal[7], cold[7], badcrc[7];
        make_frame(room, 0x18, 23.0f, 70.0f);
        make_frame(room2, 0x18, 23.2f, 69.0f);
        make_frame(breath, 0x18, 23.4f, 78.0f);         /* breathed on: +8 % */
        make_frame(busy, 0x98, 23.0f, 70.0f);
        make_frame(nocal, 0x10, 23.0f, 70.0f);          /* CAL clear, CRC fine */
        make_frame(cold, 0x18, -42.6f, 70.0f);          /* below the part's range */
        make_frame(badcrc, 0x18, 23.0f, 70.0f);
        badcrc[6] ^= 0x01;

        /* A healthy sensor. Its first frame stands alone, so it asks for a
           second straight away; that one agrees and is used. */
        memset(&v, 0, sizeof v);
        expect("a first good frame is not used alone",
               aht21_vet(&v, room, &c, &rh) == AHT21_CONFIRM);
        expect("an agreeing frame straight after it is",
               aht21_vet(&v, room2, &c, &rh) == AHT21_USE
               && fabsf(c - 23.2f) < 0.01f && fabsf(rh - 69.0f) < 0.01f);
        expect("and so is the next, thirty seconds on",
               aht21_vet(&v, room, &c, &rh) == AHT21_USE);

        /* A real jump -- a breath -- costs a second measurement, not the
           reading. */
        expect("a real jump is asked about again",
               aht21_vet(&v, breath, &c, &rh) == AHT21_CONFIRM);
        expect("and used once a second frame agrees",
               aht21_vet(&v, breath, &c, &rh) == AHT21_USE && fabsf(rh - 78.0f) < 0.01f);

        /* One chance pass among good frames: it disagrees with the frame
           before, the frame measured to confirm it fails its CRC, and it is
           never used. */
        memset(&v, 0, sizeof v);
        aht21_vet(&v, room, &c, &rh);
        aht21_vet(&v, room, &c, &rh);
        c = rh = -99.0f;
        expect("a chance pass after good frames is not used",
               aht21_vet(&v, bench[0], &c, &rh) == AHT21_CONFIRM);
        expect("and its confirming frame, failing, drops it",
               aht21_vet(&v, badcrc, &c, &rh) == AHT21_DROP);
        expect("so 0 C was never handed out", c == -99.0f && rh == -99.0f);
        /* The next good frame has a failed one before it, not a good one. */
        expect("a good frame after a failed one is confirmed, not trusted",
               aht21_vet(&v, room, &c, &rh) == AHT21_CONFIRM);
        expect("and then used", aht21_vet(&v, room, &c, &rh) == AHT21_USE);

        /* Two good frames with a failed one between are not consecutive. */
        memset(&v, 0, sizeof v);
        aht21_vet(&v, room, &c, &rh);
        aht21_vet(&v, badcrc, &c, &rh);
        expect("agreement across a failed frame does not count",
               aht21_vet(&v, room, &c, &rh) == AHT21_CONFIRM);

        /* The target state: every frame damaged. Even two chance passes in a
           row that agree with each other -- 0.00 C and 0.02 C, both near the
           zero the damage drags bytes towards -- are not used. */
        memset(&v, 0, sizeof v);
        for (int i = 0; i < 20; i++) aht21_vet(&v, badcrc, &c, &rh);
        expect("a sensor failing every frame is not trusted", !aht21_vet_trusted(&v));
        uint8_t near0[7] = { 0x18, 0x30, 0x01, 0xd4, 0x00, 0x60, 0 };  /* 0.01 C */
        near0[6] = aht21_crc(near0, 6);
        c = rh = -99.0f;
        int used = 0;
        used += aht21_vet(&v, bench[0], &c, &rh) == AHT21_USE;
        used += aht21_vet(&v, near0, &c, &rh) == AHT21_USE;
        for (int i = 0; i < 4; i++) used += aht21_vet(&v, bench[i], &c, &rh) == AHT21_USE;
        expect("so no chance pass is used, even two that agree",
               used == 0 && c == -99.0f);

        /* Recovery: once most of the recent frames are good again, it is. */
        for (int i = 0; i < AHT21_HISTORY; i++) aht21_vet(&v, room, &c, &rh);
        expect("a sensor that recovers is trusted again", aht21_vet_trusted(&v));
        expect("and its readings used", aht21_vet(&v, room2, &c, &rh) == AHT21_USE);

        /* The gate sits at half: more than half failed is distrust. Good
           frames first, so each failure after them pushes a good one out. */
        memset(&v, 0, sizeof v);
        for (int i = 0; i < AHT21_HISTORY; i++)
            aht21_vet(&v, i < AHT21_HISTORY / 2 ? room : badcrc, &c, &rh);
        expect("half the recent frames failing is still trusted", aht21_vet_trusted(&v));
        aht21_vet(&v, badcrc, &c, &rh);
        expect("a majority failing is not", !aht21_vet_trusted(&v));
        /* At boot, with fewer frames than the window, the rate is over what
           there is. */
        memset(&v, 0, sizeof v);
        aht21_vet(&v, badcrc, &c, &rh);
        expect("one failure of one is a majority", !aht21_vet_trusted(&v));
        aht21_vet(&v, room, &c, &rh);
        expect("one of two is not", aht21_vet_trusted(&v));

        /* What a CRC cannot see. */
        memset(&v, 0, sizeof v);
        aht21_vet(&v, room, &c, &rh);
        expect("a frame without the calibrated bit is dropped",
               aht21_vet(&v, nocal, &c, &rh) == AHT21_DROP);
        memset(&v, 0, sizeof v);
        aht21_vet(&v, room, &c, &rh);
        expect("as is one outside the part's rated range",
               aht21_vet(&v, cold, &c, &rh) == AHT21_DROP);
        memset(&v, 0, sizeof v);
        aht21_vet(&v, room, &c, &rh);               /* a neighbour, now */
        expect("a busy frame is no reading either",
               aht21_vet(&v, busy, &c, &rh) == AHT21_DROP);
        expect("and breaks the chain, so the next sound frame is only confirmed",
               aht21_vet(&v, room, &c, &rh) == AHT21_CONFIRM);
        expect("but is not counted as damage", aht21_vet_trusted(&v) && v.failed == 0);

        /* The bench frames: three of the four also fall outside a room, but
           the fourth, 24.95 C and 20 %, is a room reading in every respect a
           band could test. Agreement is what stops it. */
        memset(&v, 0, sizeof v);
        aht21_vet(&v, room, &c, &rh);
        aht21_vet(&v, room, &c, &rh);
        expect("a room-shaped chance pass is caught by disagreement",
               aht21_vet(&v, bench[3], &c, &rh) == AHT21_CONFIRM);
    }

    printf("%s\n", failures ? "FAILURES" : "all tests passed");
    return failures ? 1 : 0;
}
