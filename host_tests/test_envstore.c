#include "envstore.h"

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
 * A fake NOR flash that behaves like the real thing rather than like RAM.
 *
 * Two rules matter, and both have to be enforced or the tests pass on
 * hardware that would fail: a write may only turn bits from 1 to 0, and only
 * an erase of a whole sector puts them back. A ring buffer that quietly
 * rewrites a byte works perfectly in RAM and returns garbage on the board.
 */
#define SECTOR 4096
#define SECTORS 8
#define SIZE (SECTOR * SECTORS)

typedef struct {
    uint8_t mem[SIZE];
    int erases[SECTORS];
    int bad_writes;          /* writes that tried to raise a bit */
    int fail_write_after;    /* -1 off; otherwise fail once the count hits 0 */
} fake_t;

static bool f_read(void *ctx, size_t off, void *dst, size_t len)
{
    fake_t *f = ctx;
    if (off + len > SIZE) return false;
    memcpy(dst, f->mem + off, len);
    return true;
}

static bool f_write(void *ctx, size_t off, const void *src, size_t len)
{
    fake_t *f = ctx;
    const uint8_t *s = src;
    if (off + len > SIZE) return false;
    if (f->fail_write_after >= 0 && f->fail_write_after-- == 0) return false;
    for (size_t i = 0; i < len; i++) {
        /* The rule: a bit that is already 0 cannot be raised by a write. */
        if ((f->mem[off + i] & s[i]) != s[i]) f->bad_writes++;
        f->mem[off + i] &= s[i];
    }
    return true;
}

static bool f_erase(void *ctx, size_t off, size_t len)
{
    fake_t *f = ctx;
    if (off % SECTOR || len % SECTOR || off + len > SIZE) return false;
    memset(f->mem + off, 0xFF, len);
    for (size_t i = off; i < off + len; i += SECTOR) f->erases[i / SECTOR]++;
    return true;
}

static fake_t fake;
static envflash_t flash;

static void fresh(void)
{
    memset(&fake, 0xFF, sizeof fake.mem);
    memset(fake.erases, 0, sizeof fake.erases);
    fake.bad_writes = 0;
    fake.fail_write_after = -1;
    flash.ctx = &fake;
    flash.size = SIZE;
    flash.sector_size = SECTOR;
    flash.read = f_read;
    flash.write = f_write;
    flash.erase = f_erase;
}

static env_sample_t made(uint32_t minute)
{
    env_sample_t s;
    s.minute = minute;
    s.temp_c100 = (int16_t)(2000 + (minute % 500));
    s.rh_c100 = (uint16_t)(4000 + (minute % 100));
    s.hpa_x10 = (uint16_t)(9800 + (minute % 50));
    s.tvoc_ppb = (uint16_t)(minute % 1000);
    s.eco2_ppm = (uint16_t)(400 + (minute % 600));
    s.aqi = (uint8_t)(1 + (minute % 5));
    s.flags = ENV_HAVE_TEMP | ENV_HAVE_RH | ENV_HAVE_HPA | ENV_HAVE_GAS;
    return s;
}

/* Collects a walk into an array so it can be checked in order. */
typedef struct { env_sample_t *v; int n, max; } collect_t;
static bool collect(const env_sample_t *s, void *ctx)
{
    collect_t *c = ctx;
    if (c->n < c->max) c->v[c->n++] = *s;
    return true;
}

static uint8_t buf[SECTOR];

static int commas(const char *s)
{
    int n = 0;
    for (; *s; s++) n += *s == ',';
    return n;
}

/* A card, as envcsv_path sees it: some files, each with a first line. */
typedef struct {
    const char *path[ENVCSV_MAX_FILES + 1];
    const char *first[ENVCSV_MAX_FILES + 1];
    int         n;
    bool        broken;     /* the card cannot be read at all */
} fakefs_t;

static void fakefs_add(fakefs_t *fs, const char *path, const char *first)
{
    fs->path[fs->n] = path;
    fs->first[fs->n] = first;
    fs->n++;
}

static int fake_head(const char *path, char *line, size_t size, void *ctx)
{
    fakefs_t *fs = ctx;
    if (fs->broken) return -1;
    for (int i = 0; i < fs->n; i++)
        if (strcmp(fs->path[i], path) == 0) {
            snprintf(line, size, "%s", fs->first[i]);
            return 1;
        }
    return 0;
}

int main(void)
{
    envstore_t s;

    fresh();
    expect("a blank partition formats itself", envstore_open(&s, &flash));
    expect("and starts empty", envstore_count(&s) == 0);
    expect("with a capacity of every slot in every sector",
           envstore_capacity(&s) == SECTORS * ((SECTOR - ENVSTORE_HDR) / ENVSTORE_RECORD));

    env_sample_t got;
    expect("an empty log has no latest reading", !envstore_latest(&s, &got));

    /* Round-tripping, exactly: this is a log read back a month later, so
       every field has to survive the encoding unchanged. */
    env_sample_t one = { 12345, -1234, 8765, 9847, 420, 650, 2,
                         ENV_HAVE_TEMP | ENV_HAVE_RH | ENV_HAVE_HPA | ENV_HAVE_GAS };
    expect("one reading goes in", envstore_add(&s, &one));
    expect("and comes back", envstore_latest(&s, &got));
    expect("with every field intact",
           got.minute == one.minute && got.temp_c100 == one.temp_c100
           && got.rh_c100 == one.rh_c100 && got.hpa_x10 == one.hpa_x10
           && got.tvoc_ppb == one.tvoc_ppb && got.eco2_ppm == one.eco2_ppm
           && got.aqi == one.aqi && got.flags == one.flags);

    /*
     * A board that cannot measure something says so, rather than storing a
     * zero that reads back as a real reading. The log outlives the board that
     * wrote it, and "no barometer" and "1013 hPa" are different facts.
     */
    expect("what was measured is recorded as measured",
           (got.flags & ENV_HAVE_GAS) && (got.flags & ENV_HAVE_HPA));
    expect("and the gas validity survives with it",
           ENV_GAS_VALIDITY(got.flags) == 0);
    expect("and is counted", envstore_count(&s) == 1);

    /* A board with no barometer stores no pressure, and says so. */
    {
        /* Gas readings taken in the first hour from cold are marked as such:
           validity 2 means "still settling", not "the air is like this". */
        env_sample_t dry = { 12347, 2200, 5000, 0, 310, 700, 3,
                             ENV_HAVE_TEMP | ENV_HAVE_RH | ENV_HAVE_GAS
                             | ENV_GAS_FLAGS(2) };
        envstore_add(&s, &dry);
        envstore_latest(&s, &got);
        expect("a missing sensor reads back as missing, not as zero",
               !(got.flags & ENV_HAVE_HPA) && (got.flags & ENV_HAVE_GAS));
        expect("and a settling gas reading keeps its validity",
               ENV_GAS_VALIDITY(got.flags) == 2);
        expect("without disturbing which sensors were present",
               (got.flags & ENV_HAVE_TEMP) && (got.flags & ENV_HAVE_RH)
               && !(got.flags & ENV_HAVE_HPA));
    }

    /* Negative temperatures: this board could end up in a shed. */
    env_sample_t cold = { 12346, -3000, 9000, 10200, 0, 0, 0, ENV_HAVE_TEMP };
    envstore_add(&s, &cold);
    expect("a freezing reading survives the round trip",
           envstore_latest(&s, &got) && got.temp_c100 == -3000);

    /* Filling past one sector must roll into the next. */
    fresh();
    envstore_open(&s, &flash);
    int per = envstore_capacity(&s) / SECTORS;
    for (int i = 0; i < per + 5; i++) {
        env_sample_t x = made((uint32_t)i);
        if (!envstore_add(&s, &x)) { printf("FAIL add %d\n", i); failures++; break; }
    }
    expect("it crosses into the next sector", envstore_count(&s) == per + 5);
    expect("and the newest is still the newest",
           envstore_latest(&s, &got) && got.minute == (uint32_t)(per + 4));

    /* Order is oldest first, which is what a chart draws left to right. */
    {
        static env_sample_t all[SECTORS * 400];
        collect_t c = { all, 0, SECTORS * 400 };
        envstore_walk(&s, buf, collect, &c);
        expect("the walk sees every reading", c.n == per + 5);
        int ordered = 1;
        for (int i = 0; i < c.n; i++) if (all[i].minute != (uint32_t)i) ordered = 0;
        expect("oldest first, in order", ordered);
    }

    /*
     * The ring. Filling it past capacity must drop the oldest and keep going,
     * which is the whole thirty-day retention, and the walk must still come
     * back in order across the wrap -- the place a ring buffer usually breaks.
     */
    fresh();
    envstore_open(&s, &flash);
    int cap = envstore_capacity(&s);
    for (int i = 0; i < cap + per + 7; i++) {
        env_sample_t x = made((uint32_t)i);
        envstore_add(&s, &x);
    }
    expect("a full ring never exceeds its capacity", envstore_count(&s) <= cap);
    expect("and is nearly full", envstore_count(&s) > cap - per - 1);
    {
        static env_sample_t all[SECTORS * 400];
        collect_t c = { all, 0, SECTORS * 400 };
        envstore_walk(&s, buf, collect, &c);
        expect("the wrapped walk is still in order", c.n > 0
               && all[0].minute < all[c.n - 1].minute);
        int ordered = 1;
        for (int i = 1; i < c.n; i++) if (all[i].minute != all[i - 1].minute + 1) ordered = 0;
        expect("with no gaps or repeats across the wrap", ordered);
        expect("the oldest readings have been dropped", all[0].minute > 0);
        expect("and the newest is the last one written",
               all[c.n - 1].minute == (uint32_t)(cap + per + 6));
    }

    /* Never rewrite a byte without erasing: the rule the fake enforces. */
    expect("no write ever tried to raise a bit", fake.bad_writes == 0);

    /* Wear: each sector erased about once per pass round the ring. Anything
       that erases per write would show up here as a huge number. */
    {
        int most = 0;
        for (int i = 0; i < SECTORS; i++) if (fake.erases[i] > most) most = fake.erases[i];
        expect("no sector is erased more than a few times a pass", most <= 4);
    }

    /*
     * Power cuts. Re-opening must find exactly what was written and carry on
     * from there -- this is what happens every time the board is unplugged,
     * so it is the ordinary case rather than the exotic one.
     */
    fresh();
    envstore_open(&s, &flash);
    for (int i = 0; i < per + 3; i++) { env_sample_t x = made((uint32_t)i); envstore_add(&s, &x); }
    {
        envstore_t again;
        expect("it reopens", envstore_open(&again, &flash));
        expect("finding everything that was written", envstore_count(&again) == per + 3);
        expect("and the right newest reading",
               envstore_latest(&again, &got) && got.minute == (uint32_t)(per + 2));
        env_sample_t more = made(9999);
        expect("and appends after it", envstore_add(&again, &more));
        expect("without losing the earlier ones", envstore_count(&again) == per + 4);
    }

    /* A write that fails part-way leaves a slot that looks erased, and the
       next open must treat it as free rather than as data. */
    fresh();
    envstore_open(&s, &flash);
    for (int i = 0; i < 10; i++) { env_sample_t x = made((uint32_t)i); envstore_add(&s, &x); }
    fake.fail_write_after = 0;
    {
        env_sample_t x = made(10);
        expect("a failing write is reported", !envstore_add(&s, &x));
    }
    fake.fail_write_after = -1;
    {
        envstore_t again;
        envstore_open(&again, &flash);
        expect("and leaves the log intact", envstore_count(&again) == 10);
        env_sample_t x = made(11);
        expect("ready to be written to again", envstore_add(&again, &x));
        expect("with the new reading last",
               envstore_latest(&again, &got) && got.minute == 11);
    }

    /* Garbage in the partition must be formatted rather than trusted. */
    fresh();
    memset(fake.mem, 0x5A, sizeof fake.mem);
    expect("an unrecognisable partition is reformatted", envstore_open(&s, &flash));
    expect("and comes up empty", envstore_count(&s) == 0);

    /* The erased-state timestamp cannot be stored: it is how an empty slot is
       recognised, so a reading claiming it would truncate the log. */
    {
        env_sample_t bad = { ENVSTORE_NO_MINUTE, 0, 0, 0, 0, 0, 0, 0 };
        expect("a reading cannot claim the erased timestamp", !envstore_add(&s, &bad));
    }

    /*
     * A reading with gas and nothing else: envo with the barometer pulled and
     * the hygrometer failing its CRC. It must be stored -- the gas is the
     * point of the board -- and it must read back with no temperature, not a
     * temperature of zero.
     */
    {
        fresh();
        envstore_open(&s, &flash);
        env_sample_t gas = { 20000, 0, 0, 0, 85, 512, 2,
                             ENV_HAVE_GAS | ENV_GAS_FLAGS(1) };
        expect("a gas-only reading is stored", envstore_add(&s, &gas));
        expect("and comes back gas-only", envstore_latest(&s, &got)
               && got.flags == (ENV_HAVE_GAS | ENV_GAS_FLAGS(1)));

        int16_t v = 12345;
        expect("it has no temperature", !env_sample_value(&got, ENV_TEMP, &v));
        expect("and asking does not invent one", v == 12345);
        expect("nor a humidity", !env_sample_value(&got, ENV_RH, &v));
        expect("nor a pressure", !env_sample_value(&got, ENV_HPA, &v));
        expect("but it has its VOCs",
               env_sample_value(&got, ENV_VOC, &v) && v == 85);
        expect("and its eCO2", env_sample_value(&got, ENV_CO2, &v) && v == 512);
        expect("an unknown series is never present",
               !env_sample_value(&got, ENV_SERIES, &v)
               && !env_sample_value(&got, -1, &v));

        /* Each flag unlocks exactly its own series. */
        env_sample_t t = { 20001, -250, 6500, 10132, 0, 0, 0, ENV_HAVE_TEMP };
        expect("a temperature-only reading has its temperature, sign and all",
               env_sample_value(&t, ENV_TEMP, &v) && v == -250);
        expect("and nothing else",
               !env_sample_value(&t, ENV_RH, &v) && !env_sample_value(&t, ENV_HPA, &v)
               && !env_sample_value(&t, ENV_VOC, &v) && !env_sample_value(&t, ENV_CO2, &v));
        t.flags = ENV_HAVE_TEMP | ENV_HAVE_HPA;
        expect("a barometer adds only pressure",
               env_sample_value(&t, ENV_HPA, &v) && v == 10132
               && !env_sample_value(&t, ENV_RH, &v));
        expect("the value may be skipped", env_sample_value(&t, ENV_TEMP, NULL));

        /*
         * What the charts do with it. A day of 21 C readings with gas-only
         * readings among them -- the hygrometer failing on and off -- must
         * chart 21 C to 21 C, not 0 C to 21 C. This is the fold main.c does,
         * reduced to its range.
         */
        fresh();
        envstore_open(&s, &flash);
        for (int i = 0; i < 40; i++) {
            env_sample_t r = { (uint32_t)(30000 + i), 2100, 6800, 0, 60, 450, 1,
                               ENV_HAVE_TEMP | ENV_HAVE_RH | ENV_HAVE_GAS };
            if (i % 3 == 0) {
                r.temp_c100 = 0; r.rh_c100 = 0;
                r.flags = ENV_HAVE_GAS;
            }
            envstore_add(&s, &r);
        }
        static env_sample_t all[64];
        collect_t c = { all, 0, 64 };
        envstore_walk(&s, buf, collect, &c);
        int16_t lo = 0, hi = 0;
        int temps = 0, gases = 0;
        for (int i = 0; i < c.n; i++) {
            int16_t x;
            if (env_sample_value(&all[i], ENV_TEMP, &x)) {
                if (temps == 0 || x < lo) lo = x;
                if (temps == 0 || x > hi) hi = x;
                temps++;
            }
            if (env_sample_value(&all[i], ENV_VOC, &x)) gases++;
        }
        expect("every reading is walked", c.n == 40);
        expect("the gas series is unbroken", gases == 40);
        expect("the temperature series skips the gaps", temps == 40 - 14);
        expect("and never dips to 0 C", lo == 2100 && hi == 2100);
    }

    /*
     * envo's SD log: every line has exactly the columns its file's header
     * names, whatever the sensors gave.
     */
    {
        int header_commas = commas(ENVCSV_HEADER);
        expect("the header names eight columns", header_commas == 7);
        expect("and is one line", strchr(ENVCSV_HEADER, '\n')
               == ENVCSV_HEADER + strlen(ENVCSV_HEADER) - 1);

        const uint8_t parts[4] = { ENV_HAVE_TEMP, ENV_HAVE_RH, ENV_HAVE_HPA, ENV_HAVE_GAS };
        int mismatched = 0, unterminated = 0;
        for (int m = 0; m < 32; m++) {
            env_sample_t r = { 0, 2250, 6800, 10132, 85, 512, 2, 0 };
            for (int i = 0; i < 4; i++) if (m & (1 << i)) r.flags |= parts[i];
            char line[128];
            int n = envcsv_line(line, sizeof line, &r, 2026, 9, 24, 13 * 3600 + 5 * 60 + 30,
                                (m & 16) != 0, 31.5f);
            if (commas(line) != header_commas) mismatched++;
            if (n <= 0 || line[n - 1] != '\n' || strchr(line, '\n') != line + n - 1)
                unterminated++;
        }
        expect("every combination of sensors gives the header's column count",
               mismatched == 0);
        expect("each as one whole line", unterminated == 0);

        env_sample_t full = { 0, -250, 6800, 10132, 85, 512, 2,
                              ENV_HAVE_TEMP | ENV_HAVE_RH | ENV_HAVE_HPA | ENV_HAVE_GAS
                              | ENV_GAS_FLAGS(1) };
        char line[128];
        envcsv_line(line, sizeof line, &full, 2026, 9, 24, 13 * 3600 + 5 * 60 + 30, true, 31.5f);
        expect("a full line reads as it always did, sign and all",
               strcmp(line, "2026-09-24T13:05:30,-2.50,68.00,1013.2,85,512,31.5,1\n") == 0);
        env_sample_t gas = { 0, 0, 0, 0, 85, 512, 2, ENV_HAVE_GAS };
        envcsv_line(line, sizeof line, &gas, 2026, 9, 24, 59, false, 0.0f);
        expect("a gas-only line leaves the rest empty, not zero",
               strcmp(line, "2026-09-24T00:00:59,,,,85,512,,0\n") == 0);
    }

    /*
     * Which file a day's lines go in. The header is written only when a file
     * is made, so a file begun by firmware with other columns -- the day this
     * is flashed, the old seven-column header already on the card -- must not
     * be appended to: every line would carry a column its header lacks.
     */
    {
        static const char OLD[] =
            "timestamp,temp_c,rh_pct,pressure_hpa,tvoc_ppb,eco2_ppm,die_c";
        char now_hdr[128];
        snprintf(now_hdr, sizeof now_hdr, "%.*s",
                 (int)strlen(ENVCSV_HEADER) - 1, ENVCSV_HEADER);
        char path[48];
        bool fresh = false;
        fakefs_t fs;

        memset(&fs, 0, sizeof fs);
        expect("a new day gets the plain name",
               envcsv_path(path, sizeof path, 2026, 9, 24, fake_head, &fs, &fresh)
               && strcmp(path, "envo/2026-09-24.csv") == 0 && fresh);

        fakefs_add(&fs, "envo/2026-09-24.csv", now_hdr);
        expect("a file with today's columns is carried on",
               envcsv_path(path, sizeof path, 2026, 9, 24, fake_head, &fs, &fresh)
               && strcmp(path, "envo/2026-09-24.csv") == 0 && !fresh);

        memset(&fs, 0, sizeof fs);
        fakefs_add(&fs, "envo/2026-09-24.csv", OLD);
        expect("a file begun with other columns is left alone",
               envcsv_path(path, sizeof path, 2026, 9, 24, fake_head, &fs, &fresh)
               && strcmp(path, "envo/2026-09-24_2.csv") == 0 && fresh);
        expect("and its successor sorts after it, as it was written",
               strcmp("envo/2026-09-24.csv", path) < 0);
        fakefs_add(&fs, "envo/2026-09-24_2.csv", now_hdr);
        expect("and after a reboot the same day, its successor is found again",
               envcsv_path(path, sizeof path, 2026, 9, 24, fake_head, &fs, &fresh)
               && strcmp(path, "envo/2026-09-24_2.csv") == 0 && !fresh);

        memset(&fs, 0, sizeof fs);
        fakefs_add(&fs, "envo/2026-09-24.csv", "");
        expect("an empty file wants its header",
               envcsv_path(path, sizeof path, 2026, 9, 24, fake_head, &fs, &fresh)
               && strcmp(path, "envo/2026-09-24.csv") == 0 && fresh);

        memset(&fs, 0, sizeof fs);
        fs.broken = true;
        expect("a card that cannot be read chooses nothing",
               !envcsv_path(path, sizeof path, 2026, 9, 24, fake_head, &fs, &fresh));

        memset(&fs, 0, sizeof fs);
        for (int i = 1; i <= ENVCSV_MAX_FILES; i++) {
            static char names[ENVCSV_MAX_FILES + 1][32];
            if (i == 1) snprintf(names[i], sizeof names[i], "envo/2026-09-24.csv");
            else snprintf(names[i], sizeof names[i], "envo/2026-09-24_%d.csv", i);
            fakefs_add(&fs, names[i], OLD);
        }
        expect("and every name taken by other columns is no name at all",
               !envcsv_path(path, sizeof path, 2026, 9, 24, fake_head, &fs, &fresh));
    }

    printf("%s\n", failures ? "FAILURES" : "all tests passed");
    return failures ? 1 : 0;
}
