#include "envstate.h"

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
 * Time as the firmware has it, in microseconds from boot. The first reading
 * is a hundred seconds in rather than at zero, so nothing here can pass by
 * confusing a real timestamp with the "no gas reading yet" that
 * gas_start_us starts as.
 */
#define S      1000000LL
#define MIN    (60 * S)
#define STEP   (30 * S)             /* the sampler's cadence */
#define T0     (100 * S)

/* Static, as the firmware's will be: a ring of 96 is a couple of KB, and every
   test starts it from envs_init rather than trusting what the last one left. */
static envs_t e;

static envs_reading_t reading(int64_t t_us)
{
    envs_reading_t r;
    memset(&r, 0, sizeof r);
    r.t_us = t_us;
    return r;
}

static void push_gas(int64_t t_us, uint16_t tvoc, uint16_t eco2, uint8_t validity)
{
    envs_reading_t r = reading(t_us);
    r.tvoc = tvoc;
    r.eco2 = eco2;
    r.aqi = 1;
    r.validity = validity;
    r.have = ENV_HAVE_GAS;
    envs_push(&e, &r);
}

static void push_voc(int64_t t_us, uint16_t tvoc, uint8_t validity)
{
    push_gas(t_us, tvoc, 400, validity);
}

/*
 * main.c's loop for an hour: a pass every 30 s, a 5-minute record due on
 * every tenth. On a pass with a record the fold runs first and the pass's own
 * reading is pushed after it, as env_sample and env_sdlog run. `split_at_now`
 * is the firmware's window, [from, now) then from = now; without it, the
 * window it first had, [from, now + 1) then from = now + 1. Each reading is
 * 50 ppb except the one on a record's time, 900. Returns the records written.
 */
static int replay_records(bool split_at_now, env_sample_t *rec, int max)
{
    envs_init(&e);
    int64_t from = T0;
    int n = 0;
    for (int k = 0; k <= 60; k++) {
        int64_t now = T0 + k * STEP;
        if (k > 0 && k % 10 == 0 && n < max) {
            int64_t to = split_at_now ? now : now + 1;
            memset(&rec[n], 0, sizeof rec[n]);
            if (envs_fold5(&e, from, to, &rec[n])) n++;
            from = to;
        }
        push_voc(now, k % 10 == 0 ? 900 : 50, 0);
    }
    return n;
}

static void push_room(int64_t t_us, int16_t temp_c100, uint16_t rh_c100)
{
    envs_reading_t r = reading(t_us);
    r.temp_c100 = temp_c100;
    r.rh_c100 = rh_c100;
    r.have = ENV_HAVE_TEMP | ENV_HAVE_RH;
    envs_push(&e, &r);
}

/* One valid reading of series `s` at `v`, the other fields plausible. */
static void push_value(envs_series_t s, int64_t t_us, int32_t v)
{
    switch (s) {
    case ENVS_VOC:  push_gas(t_us, (uint16_t)v, 400, 0); break;
    case ENVS_ECO2: push_gas(t_us, 0, (uint16_t)v, 0); break;
    case ENVS_TEMP: push_room(t_us, (int16_t)v, 4000); break;
    case ENVS_RH:   push_room(t_us, 2000, (uint16_t)v); break;
    default: break;
    }
}

/*
 * The trend of `n_old` readings at `old` followed by `n_new` at `new`, 30 s
 * apart, judged at the moment of the last one -- the shape of every trend
 * case below, which differ only in the numbers.
 */
static envs_trend_t trend_of(envs_series_t s, int n_old, int32_t old,
                             int n_new, int32_t new)
{
    envs_init(&e);
    int64_t t = T0;
    for (int i = 0; i < n_old; i++, t += STEP) push_value(s, t, old);
    for (int i = 0; i < n_new; i++, t += STEP) push_value(s, t, new);
    return envs_trend(&e, s, t - STEP);
}

static int32_t value_of(envs_series_t s)
{
    int32_t v = -99999;
    return envs_value(&e, s, &v) ? v : -99999;
}

int main(void)
{
    /*
     * The limits, from the ENS160 datasheet v1.3, Tables 5-6: the chip's own
     * table, because its ppb are ethanol-equivalent and only its own limits
     * mean anything against them. Spelled out as literals so a change to the
     * table has to be made twice, once on purpose.
     */
    {
        const envs_limits_t *voc = envs_limits(ENVS_VOC);
        const envs_limits_t *co2 = envs_limits(ENVS_ECO2);
        expect("VOC limits are 220 / 650, chart 0..2200",
               voc && voc->fair == 220 && voc->poor == 650
               && voc->top == 2200 && voc->floor == 0);
        expect("eCO2 limits are 800 / 1000, chart 400..1500",
               co2 && co2->fair == 800 && co2->poor == 1000
               && co2->top == 1500 && co2->floor == 400);
        expect("temperature has no limits", envs_limits(ENVS_TEMP) == NULL);
        expect("nor does humidity", envs_limits(ENVS_RH) == NULL);
        expect("nor does a series that does not exist", envs_limits(ENVS_N) == NULL);
    }

    /*
     * The state, and its hysteresis: worse at once, better only 10 % below
     * the edge. Without it, a room sitting at 219-221 ppb flips GOOD, FAIR,
     * GOOD every thirty seconds, and a word that flickers is a word nobody
     * believes.
     */
    expect("VOC 219 from OK is OK", envs_classify(ENVS_VOC, 219, ENVS_OK) == ENVS_OK);
    expect("220 is FAIR", envs_classify(ENVS_VOC, 220, ENVS_OK) == ENVS_FAIR);
    expect("649 is FAIR", envs_classify(ENVS_VOC, 649, ENVS_OK) == ENVS_FAIR);
    expect("650 is POOR", envs_classify(ENVS_VOC, 650, ENVS_OK) == ENVS_POOR);
    expect("clean air is OK", envs_classify(ENVS_VOC, 0, ENVS_OK) == ENVS_OK);
    expect("far past the chart is still just POOR",
           envs_classify(ENVS_VOC, 60000, ENVS_OK) == ENVS_POOR);

    expect("from FAIR, 199 stays FAIR", envs_classify(ENVS_VOC, 199, ENVS_FAIR) == ENVS_FAIR);
    expect("from FAIR, 198 -- exactly 0.9 x 220 -- still stays",
           envs_classify(ENVS_VOC, 198, ENVS_FAIR) == ENVS_FAIR);
    expect("from FAIR, 197 is OK", envs_classify(ENVS_VOC, 197, ENVS_FAIR) == ENVS_OK);
    expect("from FAIR, 700 is POOR at once", envs_classify(ENVS_VOC, 700, ENVS_FAIR) == ENVS_POOR);
    expect("from OK, 650 skips FAIR and is POOR at once",
           envs_classify(ENVS_VOC, 650, ENVS_OK) == ENVS_POOR);

    expect("from POOR, 590 stays POOR", envs_classify(ENVS_VOC, 590, ENVS_POOR) == ENVS_POOR);
    expect("from POOR, 585 -- exactly 0.9 x 650 -- still stays",
           envs_classify(ENVS_VOC, 585, ENVS_POOR) == ENVS_POOR);
    expect("from POOR, 584 is FAIR", envs_classify(ENVS_VOC, 584, ENVS_POOR) == ENVS_FAIR);
    /* A window opened on a bad room: the value falls past both edges in one
       step, and the word must follow it all the way down, not stop at FAIR. */
    expect("from POOR, 100 is OK", envs_classify(ENVS_VOC, 100, ENVS_POOR) == ENVS_OK);
    expect("from POOR, 197 is OK", envs_classify(ENVS_VOC, 197, ENVS_POOR) == ENVS_OK);
    expect("but from POOR, 210 is only FAIR -- inside FAIR's own band",
           envs_classify(ENVS_VOC, 210, ENVS_POOR) == ENVS_FAIR);

    /* WAIT is no state to hold on to: the first reading after warm-up is
       classified as it stands. */
    expect("from WAIT, 210 is OK", envs_classify(ENVS_VOC, 210, ENVS_WAIT) == ENVS_OK);
    expect("from WAIT, 600 is FAIR", envs_classify(ENVS_VOC, 600, ENVS_WAIT) == ENVS_FAIR);
    expect("from WAIT, 650 is POOR", envs_classify(ENVS_VOC, 650, ENVS_WAIT) == ENVS_POOR);

    expect("eCO2 799 is OK", envs_classify(ENVS_ECO2, 799, ENVS_OK) == ENVS_OK);
    expect("eCO2 800 is FAIR", envs_classify(ENVS_ECO2, 800, ENVS_OK) == ENVS_FAIR);
    expect("eCO2 999 is FAIR", envs_classify(ENVS_ECO2, 999, ENVS_OK) == ENVS_FAIR);
    expect("eCO2 1000 is POOR", envs_classify(ENVS_ECO2, 1000, ENVS_OK) == ENVS_POOR);
    expect("eCO2 at its 400 floor is OK", envs_classify(ENVS_ECO2, 400, ENVS_OK) == ENVS_OK);
    expect("eCO2 from FAIR, 720 stays FAIR", envs_classify(ENVS_ECO2, 720, ENVS_FAIR) == ENVS_FAIR);
    expect("eCO2 from FAIR, 719 is OK", envs_classify(ENVS_ECO2, 719, ENVS_FAIR) == ENVS_OK);
    expect("eCO2 from POOR, 900 stays POOR", envs_classify(ENVS_ECO2, 900, ENVS_POOR) == ENVS_POOR);
    expect("eCO2 from POOR, 899 is FAIR", envs_classify(ENVS_ECO2, 899, ENVS_POOR) == ENVS_FAIR);
    expect("eCO2 from POOR, 719 is OK", envs_classify(ENVS_ECO2, 719, ENVS_POOR) == ENVS_OK);

    /* Temperature and humidity are shown raw with no word, so they have no
       state to be in -- whatever came before. */
    expect("a hot room has no word", envs_classify(ENVS_TEMP, 4000, ENVS_POOR) == ENVS_OK);
    expect("nor does a damp one", envs_classify(ENVS_RH, 9500, ENVS_FAIR) == ENVS_OK);

    /*
     * The number on screen, truncated to a step so the last digit is not
     * sensor noise. Truncated rather than rounded, so the number never shows
     * an edge the reading has not reached: 218 is GOOD, and must not read 220.
     */
    expect("VOC 47 shows 45", envs_quantise(ENVS_VOC, 47) == 45);
    expect("VOC 218 shows 210", envs_quantise(ENVS_VOC, 218) == 210);
    expect("VOC 1366 shows 1300", envs_quantise(ENVS_VOC, 1366) == 1300);
    expect("VOC 0 shows 0", envs_quantise(ENVS_VOC, 0) == 0);
    expect("VOC 4 shows 0", envs_quantise(ENVS_VOC, 4) == 0);
    expect("VOC 99 shows 95", envs_quantise(ENVS_VOC, 99) == 95);
    expect("VOC 100 shows 100, the step now 10", envs_quantise(ENVS_VOC, 100) == 100);
    expect("VOC 999 shows 990", envs_quantise(ENVS_VOC, 999) == 990);
    expect("VOC 1000 shows 1000, the step now 100", envs_quantise(ENVS_VOC, 1000) == 1000);
    expect("VOC 1099 shows 1000", envs_quantise(ENVS_VOC, 1099) == 1000);
    expect("the chip's largest VOC shows 65500", envs_quantise(ENVS_VOC, 65535) == 65500);
    expect("eCO2 447 shows 440", envs_quantise(ENVS_ECO2, 447) == 440);
    expect("eCO2 400 shows 400", envs_quantise(ENVS_ECO2, 400) == 400);
    expect("eCO2 1509 shows 1500", envs_quantise(ENVS_ECO2, 1509) == 1500);
    expect("TEMP 26.87 shows 26.8", envs_quantise(ENVS_TEMP, 2687) == 2680);
    expect("TEMP 26.80 shows 26.8", envs_quantise(ENVS_TEMP, 2680) == 2680);
    /* Temperature is the only series that can go below zero. Truncation is
       toward zero there, as C's division is: -5.23 shows -5.2. */
    expect("TEMP -5.23 shows -5.2", envs_quantise(ENVS_TEMP, -523) == -520);
    expect("TEMP -0.05 shows 0.0, not a minus zero", envs_quantise(ENVS_TEMP, -5) == 0);
    expect("RH 37.65 shows 37", envs_quantise(ENVS_RH, 3765) == 3700);
    expect("RH 100 shows 100", envs_quantise(ENVS_RH, 10000) == 10000);
    expect("RH 0.99 shows 0", envs_quantise(ENVS_RH, 99) == 0);

    /*
     * The point of truncating, checked everywhere it matters: every edge is a
     * multiple of its step, so the number on screen is past an edge exactly
     * when the reading is. The word may still lag behind the number inside a
     * hysteresis band -- that is by design -- but the number itself never
     * claims a limit the air has not reached.
     */
    {
        bool agree = true, never_more = true;
        for (int32_t v = 0; v <= 3000; v++) {
            int32_t q = envs_quantise(ENVS_VOC, v);
            if ((q >= 220) != (v >= 220) || (q >= 650) != (v >= 650)) agree = false;
            if (q > v) never_more = false;
        }
        for (int32_t v = 400; v <= 2000; v++) {
            int32_t q = envs_quantise(ENVS_ECO2, v);
            if ((q >= 800) != (v >= 800) || (q >= 1000) != (v >= 1000)) agree = false;
            if (q > v) never_more = false;
        }
        expect("the shown number crosses an edge exactly when the reading does", agree);
        expect("and is never more than the reading", never_more);
    }

    /*
     * A fresh ring: nothing to show, nothing warming, no verdict yet. The
     * gases wait for a reading; temperature and humidity have no word to
     * wait for.
     */
    envs_init(&e);
    {
        int32_t v = 12345;
        expect("an empty ring has no VOC value", !envs_value(&e, ENVS_VOC, &v));
        expect("and leaves the caller's variable alone", v == 12345);
        expect("nor any temperature", !envs_value(&e, ENVS_TEMP, &v));
        expect("the gases start in WAIT",
               e.state[ENVS_VOC] == ENVS_WAIT && e.state[ENVS_ECO2] == ENVS_WAIT);
        expect("temperature and humidity never wait",
               e.state[ENVS_TEMP] == ENVS_OK && e.state[ENVS_RH] == ENVS_OK);
        expect("the gas has not started", e.gas_start_us < 0);
        int m = -1; bool err = true;
        expect("no gas reading is not warming up -- it is no reading",
               !envs_gas_warming(&e, &m, &err, T0));
        expect("with no minutes and no error", m == 0 && !err);
        envs_verdict_t vd = envs_verdict(&e);
        expect("the verdict waits", vd.state == ENVS_WAIT && vd.worst == ENVS_VOC);
        expect("no trend", envs_trend(&e, ENVS_VOC, T0) == ENVS_UNKNOWN);
        env_sample_t out;
        memset(&out, 0, sizeof out);
        expect("and nothing to fold", !envs_fold5(&e, 0, T0 + 10 * MIN, &out));
    }

    /*
     * The ring: 96 readings, then the oldest goes. head is the next slot to
     * write, so the newest is the one just behind it.
     */
    envs_init(&e);
    for (int i = 0; i < 100; i++) push_voc(T0 + i * STEP, (uint16_t)(i + 1), 0);
    expect("a full ring holds 96", e.n == ENVS_RING);
    expect("the newest is the hundredth",
           e.r[(e.head + ENVS_RING - 1) % ENVS_RING].tvoc == 100);
    expect("the oldest kept is the fifth", e.r[e.head].tvoc == 5);
    expect("the value is the median of the last four (97..100)", value_of(ENVS_VOC) == 98);

    /*
     * The display value: the median of the last four valid readings. A
     * median, so one spike -- a match struck, a door opening -- moves neither
     * the number nor the word; it takes three readings in a row.
     */
    envs_init(&e);
    push_voc(T0 + 0 * STEP, 10, 0);
    expect("with one valid reading, that reading", value_of(ENVS_VOC) == 10);
    push_voc(T0 + 1 * STEP, 21, 0);
    expect("with two, their mean, truncated (15)", value_of(ENVS_VOC) == 15);
    push_voc(T0 + 2 * STEP, 90, 0);
    expect("with three, the middle one", value_of(ENVS_VOC) == 21);

    envs_init(&e);
    push_voc(T0 + 0 * STEP, 10, 0);
    push_voc(T0 + 1 * STEP, 90, 0);
    push_voc(T0 + 2 * STEP, 20, 0);
    push_voc(T0 + 3 * STEP, 30, 0);
    push_voc(T0 + 4 * STEP, 1000, 2);
    expect("10 90 20 30 and an invalid 1000 give 25", value_of(ENVS_VOC) == 25);

    /* Invalid readings are gaps, wherever they fall -- even as the newest --
       and older valid readings are reached past them. */
    envs_init(&e);
    push_voc(T0 + 0 * STEP, 1000, 0);
    push_voc(T0 + 1 * STEP, 10, 0);
    push_voc(T0 + 2 * STEP, 500, 1);
    push_voc(T0 + 3 * STEP, 20, 0);
    push_voc(T0 + 4 * STEP, 600, 3);
    push_voc(T0 + 5 * STEP, 30, 0);
    push_voc(T0 + 6 * STEP, 40, 0);
    expect("only the last four valid count, skipping the invalid", value_of(ENVS_VOC) == 25);
    {
        /* A reading with a tvoc but no gas bit is a chip that did not answer:
           the field is whatever was left in it, not a measurement. */
        envs_reading_t r = reading(T0 + 7 * STEP);
        r.tvoc = 5000;
        r.have = ENV_HAVE_TEMP;
        envs_push(&e, &r);
        expect("a reading without the gas bit is no gas reading", value_of(ENVS_VOC) == 25);
    }

    envs_init(&e);
    for (int i = 0; i < 10; i++) push_voc(T0 + i * STEP, 700, (uint8_t)(1 + i % 3));
    expect("a ring of nothing but invalid gas has no value", value_of(ENVS_VOC) == -99999);

    /* Each series is its own median. */
    envs_init(&e);
    push_gas(T0 + 0 * STEP, 10, 900, 0);
    push_gas(T0 + 1 * STEP, 20, 410, 0);
    push_gas(T0 + 2 * STEP, 30, 800, 0);
    push_room(T0 + 3 * STEP, -100, 3000);
    push_room(T0 + 4 * STEP, -200, 3100);
    push_room(T0 + 5 * STEP, -300, 3300);
    push_room(T0 + 6 * STEP, -400, 3200);
    expect("eCO2 has its own median (800)", value_of(ENVS_ECO2) == 800);
    expect("VOC's is untouched by the room readings (20)", value_of(ENVS_VOC) == 20);
    expect("a cold room's median (-2.50)", value_of(ENVS_TEMP) == -250);
    expect("humidity's median (31.50)", value_of(ENVS_RH) == 3150);
    {
        int32_t v;
        expect("a series that does not exist has no value", !envs_value(&e, ENVS_N, &v));
    }

    /*
     * envs_update keeps the state, so the hysteresis is the ring's, not the
     * caller's: a VOC falling through 199 on its way down stays FAIR until it
     * is under 198.
     */
    envs_init(&e);
    for (int i = 0; i < 4; i++) push_voc(T0 + i * STEP, 300, 0);
    expect("300 ppb updates to FAIR", envs_update(&e, ENVS_VOC) == ENVS_FAIR);
    expect("and the state is kept", e.state[ENVS_VOC] == ENVS_FAIR);
    for (int i = 4; i < 8; i++) push_voc(T0 + i * STEP, 199, 0);
    expect("falling to 199 it stays FAIR", envs_update(&e, ENVS_VOC) == ENVS_FAIR);
    for (int i = 8; i < 12; i++) push_voc(T0 + i * STEP, 190, 0);
    expect("at 190 it is OK", envs_update(&e, ENVS_VOC) == ENVS_OK);
    expect("temperature with no reading is OK, never WAIT",
           envs_update(&e, ENVS_TEMP) == ENVS_OK && e.state[ENVS_TEMP] == ENVS_OK);
    expect("eCO2 at its 400 floor is OK", envs_update(&e, ENVS_ECO2) == ENVS_OK);

    /* A ring that has gone entirely invalid waits, and the first valid
       reading after it is judged afresh -- no FAIR carried across a gap. */
    envs_init(&e);
    for (int i = 0; i < 4; i++) push_voc(T0 + i * STEP, 300, 0);
    envs_update(&e, ENVS_VOC);
    for (int i = 4; i < 4 + ENVS_RING; i++) push_voc(T0 + i * STEP, 300, 3);
    expect("a ring of invalid gas is WAIT", envs_update(&e, ENVS_VOC) == ENVS_WAIT);
    push_voc(T0 + (4 + ENVS_RING) * STEP, 210, 0);
    expect("and the next valid 210 is OK, not a held FAIR",
           envs_update(&e, ENVS_VOC) == ENVS_OK);

    /*
     * The trend: the mean of the last ten minutes against the mean of the ten
     * minutes ending thirty minutes ago. Means, not the display value, so the
     * arrow answers "is it getting worse?" over a span a person notices.
     */
    expect("80 readings flat at 50, then 20 at 120, is RISING",
           trend_of(ENVS_VOC, 80, 50, 20, 120) == ENVS_RISING);
    expect("a flat line is STEADY", trend_of(ENVS_VOC, 100, 50, 0, 0) == ENVS_STEADY);
    expect("a falling line is FALLING", trend_of(ENVS_VOC, 80, 400, 20, 200) == ENVS_FALLING);
    expect("under 40 min of data is UNKNOWN", trend_of(ENVS_VOC, 60, 50, 0, 0) == ENVS_UNKNOWN);
    /* 81 readings 30 s apart reach back exactly 40 min from the last; 80
       reach back 39.5. */
    expect("39.5 min is still UNKNOWN", trend_of(ENVS_VOC, 80, 50, 0, 0) == ENVS_UNKNOWN);
    expect("40 min is enough", trend_of(ENVS_VOC, 81, 50, 0, 0) == ENVS_STEADY);

    /* VOC moves at max(15 ppb, 25 %) of where it was. */
    expect("12 ppb up from 40 is STEADY", trend_of(ENVS_VOC, 80, 40, 20, 52) == ENVS_STEADY);
    expect("15 ppb up from 40 is RISING", trend_of(ENVS_VOC, 80, 40, 20, 55) == ENVS_RISING);
    expect("14 ppb up from 0 is STEADY", trend_of(ENVS_VOC, 80, 0, 20, 14) == ENVS_STEADY);
    expect("15 ppb up from 0 is RISING", trend_of(ENVS_VOC, 80, 0, 20, 15) == ENVS_RISING);
    expect("from 200, 49 up is STEADY (under 25 %)",
           trend_of(ENVS_VOC, 80, 200, 20, 249) == ENVS_STEADY);
    expect("from 200, 50 up is RISING", trend_of(ENVS_VOC, 80, 200, 20, 250) == ENVS_RISING);
    expect("from 200, 49 down is STEADY", trend_of(ENVS_VOC, 80, 200, 20, 151) == ENVS_STEADY);
    expect("from 200, 50 down is FALLING", trend_of(ENVS_VOC, 80, 200, 20, 150) == ENVS_FALLING);
    expect("eCO2 49 ppm up is STEADY", trend_of(ENVS_ECO2, 80, 600, 20, 649) == ENVS_STEADY);
    expect("eCO2 50 ppm up is RISING", trend_of(ENVS_ECO2, 80, 600, 20, 650) == ENVS_RISING);
    expect("eCO2 50 ppm down is FALLING", trend_of(ENVS_ECO2, 80, 900, 20, 850) == ENVS_FALLING);
    expect("0.49 C up is STEADY", trend_of(ENVS_TEMP, 80, 2000, 20, 2049) == ENVS_STEADY);
    expect("0.5 C up is RISING", trend_of(ENVS_TEMP, 80, 2000, 20, 2050) == ENVS_RISING);
    expect("0.5 C down is FALLING", trend_of(ENVS_TEMP, 80, 2000, 20, 1950) == ENVS_FALLING);
    expect("below freezing, 0.5 C down is FALLING",
           trend_of(ENVS_TEMP, 80, -100, 20, -150) == ENVS_FALLING);
    expect("2.99 % up is STEADY", trend_of(ENVS_RH, 80, 4000, 20, 4299) == ENVS_STEADY);
    expect("3 % up is RISING", trend_of(ENVS_RH, 80, 4000, 20, 4300) == ENVS_RISING);
    expect("a series that does not exist has no trend",
           trend_of(ENVS_N, 100, 50, 0, 0) == ENVS_UNKNOWN);

    /* The windows are means of what is valid in them. Invalid spikes in the
       recent window change nothing; a recent window of nothing but invalid
       readings is no trend at all rather than a steady one. */
    envs_init(&e);
    {
        int64_t t = T0;
        for (int i = 0; i < 80; i++, t += STEP) push_voc(t, 50, 0);
        for (int i = 0; i < 20; i++, t += STEP) push_voc(t, i % 2 ? 50 : 1000, i % 2 ? 0 : 3);
        expect("invalid spikes among steady readings are STEADY",
               envs_trend(&e, ENVS_VOC, t - STEP) == ENVS_STEADY);
    }
    envs_init(&e);
    {
        int64_t t = T0;
        for (int i = 0; i < 80; i++, t += STEP) push_voc(t, 50, 0);
        for (int i = 0; i < 20; i++, t += STEP) push_voc(t, 1000, 3);
        expect("a recent window of only invalid readings is UNKNOWN",
               envs_trend(&e, ENVS_VOC, t - STEP) == ENVS_UNKNOWN);
        expect("so is a trend asked for twenty minutes after the last reading",
               envs_trend(&e, ENVS_VOC, t + 20 * MIN) == ENVS_UNKNOWN);
    }
    envs_init(&e);
    {
        /* The old window, 30-40 min ago, was a warm-up: nothing to compare. */
        int64_t t = T0;
        for (int i = 0; i < 15; i++, t += STEP) push_voc(t, 50, 0);
        for (int i = 0; i < 25; i++, t += STEP) push_voc(t, 50, 2);
        for (int i = 0; i < 56; i++, t += STEP) push_voc(t, 50, 0);
        expect("a gap across the old window is UNKNOWN",
               envs_trend(&e, ENVS_VOC, t - STEP) == ENVS_UNKNOWN);
    }
    envs_init(&e);
    {
        /* The case that decided "40 min" means 40 min of time rather than 80
           readings: a hygrometer failing one CRC in five must not lose its
           arrow for good. */
        int64_t t = T0;
        for (int i = 0; i < ENVS_RING; i++, t += STEP) {
            if (i % 5 == 4) {
                envs_reading_t r = reading(t);
                r.tvoc = 50;
                r.have = ENV_HAVE_GAS;
                envs_push(&e, &r);
            } else {
                push_room(t, i < 76 ? 2000 : 2100, 4000);
            }
        }
        expect("a temperature missing every fifth reading still has a trend",
               envs_trend(&e, ENVS_TEMP, t - STEP) == ENVS_RISING);
    }

    /*
     * The verdict: the worse of the two gases, VOC named on a tie. eCO2 is
     * an estimate made from the same VOC signal, so it may make the verdict
     * worse but never better.
     */
    envs_init(&e);
    {
        envs_verdict_t v;
        e.state[ENVS_VOC] = ENVS_OK; e.state[ENVS_ECO2] = ENVS_FAIR;
        v = envs_verdict(&e);
        expect("VOC OK, eCO2 FAIR: ECO2 FAIR", v.worst == ENVS_ECO2 && v.state == ENVS_FAIR);
        e.state[ENVS_VOC] = ENVS_POOR; e.state[ENVS_ECO2] = ENVS_POOR;
        v = envs_verdict(&e);
        expect("both POOR: VOC POOR", v.worst == ENVS_VOC && v.state == ENVS_POOR);
        e.state[ENVS_VOC] = ENVS_POOR; e.state[ENVS_ECO2] = ENVS_OK;
        v = envs_verdict(&e);
        expect("VOC POOR, eCO2 OK: VOC POOR -- eCO2 never makes it better",
               v.worst == ENVS_VOC && v.state == ENVS_POOR);
        e.state[ENVS_VOC] = ENVS_FAIR; e.state[ENVS_ECO2] = ENVS_POOR;
        v = envs_verdict(&e);
        expect("VOC FAIR, eCO2 POOR: ECO2 POOR", v.worst == ENVS_ECO2 && v.state == ENVS_POOR);
        e.state[ENVS_VOC] = ENVS_FAIR; e.state[ENVS_ECO2] = ENVS_FAIR;
        v = envs_verdict(&e);
        expect("both FAIR: VOC FAIR", v.worst == ENVS_VOC && v.state == ENVS_FAIR);
        e.state[ENVS_VOC] = ENVS_OK; e.state[ENVS_ECO2] = ENVS_OK;
        v = envs_verdict(&e);
        expect("both OK: AIR GOOD, named VOC", v.worst == ENVS_VOC && v.state == ENVS_OK);
        e.state[ENVS_VOC] = ENVS_WAIT; e.state[ENVS_ECO2] = ENVS_POOR;
        v = envs_verdict(&e);
        expect("no VOC state: WAIT, whatever eCO2 says", v.state == ENVS_WAIT);
        e.state[ENVS_VOC] = ENVS_POOR; e.state[ENVS_ECO2] = ENVS_WAIT;
        v = envs_verdict(&e);
        expect("no eCO2 state cannot hide a POOR VOC",
               v.worst == ENVS_VOC && v.state == ENVS_POOR);
    }

    /* End to end: readings in, states updated, verdict out. */
    envs_init(&e);
    for (int i = 0; i < 4; i++) push_gas(T0 + i * STEP, 100, 850, 0);
    envs_update(&e, ENVS_VOC);
    envs_update(&e, ENVS_ECO2);
    {
        envs_verdict_t v = envs_verdict(&e);
        expect("clean VOC with an eCO2 of 850 reads ECO2 FAIR",
               v.worst == ENVS_ECO2 && v.state == ENVS_FAIR);
    }
    /* The chip drops back into warm-up (a brown-out restarts it): the newest
       gas reading says so, and the verdict waits even though the ring still
       holds older valid readings and the states they gave. */
    push_gas(T0 + 4 * STEP, 0, 400, 1);
    {
        envs_verdict_t v = envs_verdict(&e);
        expect("a warming chip makes the verdict WAIT", v.state == ENVS_WAIT);
    }

    /*
     * Warming up. "12 MIN SO FAR", counted from the first gas reading, never
     * a time remaining: the chip's initial start-up lasts until it has run 24
     * hours unbroken, and it cannot say how far along it is.
     */
    envs_init(&e);
    push_room(T0 - STEP, 2000, 4000);
    expect("a room reading does not start the gas clock", e.gas_start_us < 0);
    push_gas(T0, 0, 400, 1);
    expect("the first gas reading does, invalid as it is", e.gas_start_us == T0);
    for (int i = 1; i < 20; i++) push_gas(T0 + i * STEP, 0, 400, 2);
    expect("and later readings leave it be", e.gas_start_us == T0);
    {
        int m = -1; bool err = true;
        int64_t now = T0 + 12 * MIN + 59 * S;
        expect("validity 2 is warming", envs_gas_warming(&e, &m, &err, now));
        expect("for (now - gas_start) / 60 s whole minutes (12)", m == 12);
        expect("and is not an error", !err);
        expect("the outputs may be left out", envs_gas_warming(&e, NULL, NULL, now));
        envs_gas_warming(&e, &m, &err, T0 - MIN);
        expect("a clock before the start is no negative minutes", m == 0);
    }
    push_room(T0 + 20 * STEP, 2000, 4000);
    {
        int m;
        expect("a newer room-only reading does not end warm-up",
               envs_gas_warming(&e, &m, NULL, T0 + 20 * STEP));
    }
    push_gas(T0 + 21 * STEP, 0, 400, 3);
    {
        int m; bool err = false;
        expect("validity 3 counts as warming, for the page",
               envs_gas_warming(&e, &m, &err, T0 + 21 * STEP));
        expect("but is an error", err);
    }
    push_gas(T0 + 22 * STEP, 40, 420, 0);
    {
        int m = -1; bool err = true;
        expect("a valid reading ends it", !envs_gas_warming(&e, &m, &err, T0 + 22 * STEP));
        expect("with no minutes and no error", m == 0 && !err);
    }
    envs_init(&e);
    push_voc(T0 + 0 * STEP, 30, 0);
    push_voc(T0 + 1 * STEP, 30, 1);
    {
        /* The display value still exists -- the median reaches past the
           invalid reading -- but the page shows WARMING UP instead. */
        expect("warming even with a value in hand",
               envs_gas_warming(&e, NULL, NULL, T0 + STEP) && value_of(ENVS_VOC) == 30);
    }

    /*
     * The 5-minute flash record: the mean of the valid 30 s readings, and the
     * peak TVOC in hpa_x10 -- envo has no barometer, and every reader asks
     * ENV_HAVE_HPA before it reads that field. A single reading every five
     * minutes, as before, was one sample of noise standing for ten.
     */
    envs_init(&e);
    {
        static const uint16_t tvoc[10] = { 100, 120, 5000, 140, 160, 180, 4000, 200, 220, 240 };
        static const uint8_t  valid[10] = { 0, 0, 2, 0, 0, 0, 3, 0, 0, 0 };
        int64_t from = T0, to = T0 + 10 * STEP;
        push_room(from - STEP, 3000, 9000);            /* before the window */
        for (int i = 0; i < 10; i++) {
            envs_reading_t r = reading(from + i * STEP);
            r.tvoc = tvoc[i];
            r.eco2 = (uint16_t)(400 + 10 * i);
            r.aqi = (uint8_t)(1 + i % 3);
            r.validity = valid[i];
            r.temp_c100 = (int16_t)(2000 + i);
            r.rh_c100 = (uint16_t)(4000 + 2 * i);
            r.have = ENV_HAVE_GAS | ENV_HAVE_TEMP | ENV_HAVE_RH;
            envs_push(&e, &r);
        }
        push_gas(to, 9999, 9999, 0);                    /* at `to`: the next window's */

        env_sample_t out;
        memset(&out, 0xA5, sizeof out);
        out.minute = 29000000u;
        bool ok = envs_fold5(&e, from, to, &out);
        expect("ten readings fold", ok);
        /* 100+120+140+160+180+200+220+240 = 1360, over 8 */
        expect("tvoc is the mean of the eight valid (170)", out.tvoc_ppb == 170);
        /* eCO2 400+10i for i = 0 1 3 4 5 7 8 9: 3200 + 370 = 3570, over 8 = 446.25 */
        expect("eco2 is the mean of the eight valid (446)", out.eco2_ppm == 446);
        expect("hpa_x10 is the valid maximum (240), not the invalid 5000",
               out.hpa_x10 == 240);
        expect("the record is marked mean + peak", (out.flags & ENV_MEANPEAK) != 0);
        expect("and does not claim a pressure", !(out.flags & ENV_HAVE_HPA));
        expect("it has gas", (out.flags & ENV_HAVE_GAS) != 0);
        expect("of validity 0 -- a mean of valid readings is valid",
               ENV_GAS_VALIDITY(out.flags) == 0);
        /* Latest, not a mean: an index of 1..5 averaged is a number the chip
           never said (the eight valid would mean 1.875). */
        expect("aqi is the latest valid reading's (i = 9)", out.aqi == 1);
        /* All ten room readings count -- gas validity is the gas's business.
           20.00..20.09 mean 20.045, 40.00..40.18 mean 40.09. */
        expect("temperature is the mean of all ten, rounded (20.05)",
               out.temp_c100 == 2005 && (out.flags & ENV_HAVE_TEMP));
        expect("humidity is the mean of all ten (40.09)",
               out.rh_c100 == 4009 && (out.flags & ENV_HAVE_RH));
        expect("the minute the caller set survives", out.minute == 29000000u);
        expect("and nothing is marked synthetic", !(out.flags & ENV_SYNTHETIC));
    }

    /* A window of nothing but warm-up: the room is still measured, and the
       record keeps the chip's latest word on its gas, which shows as a gap
       -- an hour of settling is not an hour of bad air. */
    envs_init(&e);
    {
        push_gas(T0 + 0 * STEP, 3000, 2000, 2);
        push_room(T0 + 1 * STEP, 2100, 4500);
        push_gas(T0 + 2 * STEP, 3000, 2000, 1);
        env_sample_t out;
        memset(&out, 0, sizeof out);
        expect("all-invalid gas still folds", envs_fold5(&e, T0, T0 + 5 * MIN, &out));
        expect("with the gas bit clear", !(out.flags & ENV_HAVE_GAS));
        expect("the latest validity carried (1)", ENV_GAS_VALIDITY(out.flags) == 1);
        expect("no tvoc, eco2 or peak", out.tvoc_ppb == 0 && out.eco2_ppm == 0
                                        && out.hpa_x10 == 0 && out.aqi == 0);
        expect("still mean + peak, still no pressure",
               (out.flags & ENV_MEANPEAK) && !(out.flags & ENV_HAVE_HPA));
        expect("and the room reading kept", (out.flags & ENV_HAVE_TEMP) && out.temp_c100 == 2100);
    }
    envs_init(&e);
    {
        push_gas(T0, 3000, 2000, 3);
        env_sample_t out;
        memset(&out, 0, sizeof out);
        expect("a lone invalid gas reading is still something measured",
               envs_fold5(&e, T0, T0 + 5 * MIN, &out));
        expect("carrying its validity (3) and nothing else",
               ENV_GAS_VALIDITY(out.flags) == 3
               && !(out.flags & (ENV_HAVE_GAS | ENV_HAVE_TEMP | ENV_HAVE_RH)));
    }
    envs_init(&e);
    {
        /* Gas only, no room sensor: the room fields are absent, not zero
           degrees. */
        push_voc(T0, 60, 0);
        push_voc(T0 + STEP, 70, 0);
        env_sample_t out;
        memset(&out, 0, sizeof out);
        envs_fold5(&e, T0, T0 + 5 * MIN, &out);
        expect("a gas-only window has no temperature or humidity",
               !(out.flags & (ENV_HAVE_TEMP | ENV_HAVE_RH))
               && out.temp_c100 == 0 && out.rh_c100 == 0);
        expect("and a mean of 65 with a peak of 70",
               out.tvoc_ppb == 65 && out.hpa_x10 == 70);
    }
    envs_init(&e);
    {
        /* Means round to the nearest, half away from zero: the stored record
           is a measurement, not a display, and truncating it would bias a
           month of it low. */
        push_room(T0, -101, 1);
        push_room(T0 + STEP, -100, 2);
        env_sample_t out;
        memset(&out, 0, sizeof out);
        envs_fold5(&e, T0, T0 + 5 * MIN, &out);
        expect("-1.01 and -1.00 fold to -1.01", out.temp_c100 == -101);
        expect("0.01 and 0.02 fold to 0.02", out.rh_c100 == 2);
    }
    envs_init(&e);
    {
        envs_reading_t r = reading(T0);
        envs_push(&e, &r);                              /* nothing answered */
        push_room(T0 + 10 * MIN, 2000, 4000);           /* outside the window */
        env_sample_t out;
        memset(&out, 0, sizeof out);
        expect("a window where nothing answered folds to nothing",
               !envs_fold5(&e, T0, T0 + 5 * MIN, &out));
    }

    /*
     * The firmware's loop, replayed: a record due on a pass is folded first
     * (env_sample), and that pass's own reading is pushed after it
     * (env_sdlog), both with the same `now`. Split at `now`, the reading
     * stamped at a record's time goes into the next record, and every
     * record holds its ten. A 900 ppb spike sits on each record's time, so a
     * record that lost that reading shows it twice over: a mean of 50 and a
     * peak of 50 where there should be 135 and 900.
     */
    {
        env_sample_t rec[6];
        int n = replay_records(true, rec, 6);
        int whole = 0;
        for (int i = 0; i < n; i++)
            whole += rec[i].tvoc_ppb == 135 && rec[i].hpa_x10 == 900;
        expect("six records from an hour of the loop", n == 6);
        expect("split at now, each holds its ten, the spike at its start included",
               whole == 6);

        /* The window the firmware first had, closed at now + 1 and the next
           opened there: the record-time reading fell between the two. Kept
           here as the failure the split is for. */
        n = replay_records(false, rec, 6);
        int lost = 0;
        for (int i = 1; i < n; i++)
            lost += rec[i].tvoc_ppb == 50 && rec[i].hpa_x10 == 50;
        expect("closed at now + 1, every record after the first loses that reading",
               n == 6 && lost == 5);
    }

    printf("%s\n", failures ? "FAILURES" : "all tests passed");
    return failures ? 1 : 0;
}
