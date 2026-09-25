#include "moonphase.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MOON_SECS_PER_DAY   86400.0

/* One synodic month in seconds, for stepping from one event to the next. */
#define MOON_LUNATION_SECS  (MOONPHASE_SYNODIC_DAYS * MOON_SECS_PER_DAY)

static const char *const s_names[MOONPHASE_NAME_COUNT] = {
    "New moon", "Waxing crescent", "First quarter", "Waxing gibbous",
    "Full moon", "Waning gibbous", "Last quarter", "Waning crescent",
};

const char *moonphase_name(double phase)
{
    double p = phase - floor(phase);
    /* NaN or an infinity gets here as NaN, and casting that to int is
       undefined; call it new rather than index with garbage. */
    if (!(p >= 0.0 && p < 1.0)) p = 0.0;
    return s_names[(int)(p * MOONPHASE_NAME_COUNT + 0.5) % MOONPHASE_NAME_COUNT];
}

/*
 * The first whole second at or after an instant that lies after `t`, and
 * strictly after `t` in any case.
 *
 * Rounding up is what makes "strictly after" hold: an instant anywhere in
 * the second after `t` becomes t+1, where rounding to nearest could make it
 * `t` itself. The fallback to the following month can only fire if the
 * arithmetic put the instant at `t` exactly, which integer seconds and this
 * month length should never do, but the contract is cheap to keep.
 */
static int64_t first_second_after(int64_t t, double instant)
{
    int64_t s = (int64_t)ceil(instant);
    if (s <= t) s = (int64_t)ceil(instant + MOON_LUNATION_SECS);
    return s;
}

void moonphase_compute(int64_t unix_utc, moonphase_t *out)
{
    const double synodic = MOONPHASE_SYNODIC_DAYS;

    /* Exactly almanac.py's arithmetic, in its order: whole seconds since the
       reference (exact in a double for millions of years), to days, then the
       age as days modulo the month. Python's % takes the divisor's sign and
       C's fmod the dividend's, so a time before the reference comes out
       negative here and is brought back up. The second guard is for a
       negative remainder so small that adding a month rounds it to a whole
       month, which would make the phase 1.0; it cannot happen with whole
       seconds, but the range promised in the header should hold regardless. */
    double days = (double)(unix_utc - MOONPHASE_REF_UNIX) / MOON_SECS_PER_DAY;
    double age = fmod(days, synodic);
    if (age < 0.0) age += synodic;
    if (age >= synodic) age -= synodic;

    out->age_days = age;
    out->phase = age / synodic;
    out->illuminated = (1.0 - cos(2.0 * M_PI * out->phase)) / 2.0;
    out->name = moonphase_name(out->phase);

    /*
     * The next events are counted in whole months from the reference, not
     * carried forward from `t` by the age. Carried forward, the instant came
     * out a little different, in the last bit, for every `t` in the month,
     * and an event that falls near a whole second (the reference itself
     * does, exactly) could round up to one second from one `t` and the next
     * second from another. Counted, it is the same double every time.
     *
     * The count comes from the age, so it cannot disagree with the phase
     * above: days - age is a whole number of months, less the rounding in
     * fmod, and rounding it to the nearest whole month recovers that number.
     * At age 0 the next new moon is a whole month off; once the age is past
     * the middle, the full moon to come is the next month's.
     */
    double month = floor((days - age) / synodic + 0.5);
    double full = age < synodic / 2.0 ? month + 0.5 : month + 1.5;
    out->next_new = first_second_after(
        unix_utc, (double)MOONPHASE_REF_UNIX + (month + 1.0) * MOON_LUNATION_SECS);
    out->next_full = first_second_after(
        unix_utc, (double)MOONPHASE_REF_UNIX + full * MOON_LUNATION_SECS);
}
