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
    env_sample_t one = { 12345, -1234, 8765, 9847 };
    expect("one reading goes in", envstore_add(&s, &one));
    expect("and comes back", envstore_latest(&s, &got));
    expect("with every field intact",
           got.minute == one.minute && got.temp_c100 == one.temp_c100
           && got.rh_c100 == one.rh_c100 && got.hpa_x10 == one.hpa_x10);
    expect("and is counted", envstore_count(&s) == 1);

    /* Negative temperatures: this board could end up in a shed. */
    env_sample_t cold = { 12346, -3000, 9000, 10200 };
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
        env_sample_t bad = { ENVSTORE_NO_MINUTE, 0, 0, 0 };
        expect("a reading cannot claim the erased timestamp", !envstore_add(&s, &bad));
    }

    printf("%s\n", failures ? "FAILURES" : "all tests passed");
    return failures ? 1 : 0;
}
