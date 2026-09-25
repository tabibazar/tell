#include "moonphase.h"

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

/* Quiet in the sweep, where printing every pass would bury the summary. */
static void expect_quiet(const char *what, long long t, int cond)
{
    if (cond) return;
    printf("FAIL %s at %lld\n", what, t);
    failures++;
}

#define LUNATION_SECS (MOONPHASE_SYNODIC_DAYS * 86400.0)

/*
 * The expected values were not worked out by hand: they are what
 * tools/almanac.py's moon() returns for each instant, given as a naive UTC
 * datetime, and printed to 17 significant digits. That is not what
 * almanac.py's own main() passes: it gives moon() datetime.now(), naive
 * local time, so a live `python3 tools/almanac.py` runs the UTC offset behind
 * this table and cannot be compared with it; only moon() on a UTC datetime
 * can (see moonphase.h). The illuminated fraction
 * is (1 - cos(2 pi phase)) / 2 on the same phase, and the next new and full
 * moons are almanac.py's age carried forward to the end of the month and to
 * its middle -- the model's exact instants, before rounding to a second.
 *
 * The C does the same double arithmetic in the same order, so on this host
 * it matches to the last bit. The tolerances are for another libm (newlib's
 * cos on the board), not for a different model: 1e-9 of a day is under a
 * tenth of a millisecond.
 */
typedef struct {
    long long t;
    double phase;
    double age;
    double illuminated;
    const char *name;
    double next_new;
    double next_full;
    const char *what;
} moon_case_t;

static const moon_case_t s_cases[] = {
    { 947182440LL, 0, 0, 0,
      "New moon", 949733882.877, 948458161.438,
      "the reference new moon itself, 2000-01-06 18:14:00" },
    { 947182439LL, 0.99999960806490751, 29.530577278925929, 1.5161205624281138e-12,
      "New moon", 947182440.000, 948458161.438,
      "a second before the reference" },
    { 947182441LL, 3.9193509251334377e-07, 1.1574074074074073e-05, 1.5161205624281138e-12,
      "New moon", 949733882.877, 948458161.438,
      "a second after the reference" },
    { 946684800LL, 0.80495742056165953, 23.770866630777778, 0.33075690497080479,
      "Last quarter", 947182440.000, 948458161.438,
      "2000-01-01 00:00:00, before the reference" },
    { 948458161LL, 0.49999982815621546, 14.765289351851852, 0.99999999999970857,
      "Full moon", 949733882.877, 948458161.438,
      "a second before the model's first full moon, 2000-01-21 12:36:01" },
    { 948458162LL, 0.50000022009130796, 14.765300925925926, 0.99999999999952194,
      "Full moon", 949733882.877, 951009604.315,
      "a second after the model's first full moon, 2000-01-21 12:36:02" },
    { 1118836800LL, 0.27736746691881536, 8.1908246267776157, 0.58555435538204681,
      "First quarter", 1120680555.629, 1119404834.191,
      "2005-06-15 12:00:00" },
    { 1202486132LL, 0.062476143018234037, 1.8449572925926958, 0.038031557224767754,
      "New moon", 1204878170.567, 1203602449.128,
      "a minute before the new/crescent name boundary, 2008-02-08 15:55:32" },
    { 1202486253LL, 0.062523567164426683, 1.8463577555556157, 0.038088572089433503,
      "Waxing crescent", 1204878170.567, 1203602449.128,
      "a minute after the new/crescent name boundary, 2008-02-08 15:57:33" },
    { 1356088260LL, 0.26454039094470611, 7.81203352, 0.54561646612206227,
      "First quarter", 1357964743.181, 1356689021.742,
      "2012-12-21 11:11:00" },
    { 1583044200LL, 0.21653773129759493, 6.3944867141106663, 0.39564797792489281,
      "First quarter", 1585043159.225, 1583767437.786,
      "2020-03-01 06:30:00" },
    { 1709251199LL, 0.68148956019409213, 20.124788009703529, 0.70864616284645776,
      "Waning gibbous", 1710063860.193, 1711339581.631,
      "2024-02-29 23:59:59, a leap day" },
    { 1790208000LL, 0.4113008497134229, 12.145956287776634, 0.92433962169733075,
      "Waxing gibbous", 1791710032.254, 1790434310.815,
      "today, 2026-09-24 00:00:00" },
    { 1790265600LL, 0.43387631104223257, 12.812622954444514, 0.95746388661754689,
      "Waxing gibbous", 1791710032.254, 1790434310.815,
      "today, 2026-09-24 16:00:00" },
    { 1790274845LL, 0.4374997509725234, 12.919625269259477, 0.9619394668654151,
      "Waxing gibbous", 1791710032.254, 1790434310.815,
      "a second before today's gibbous/full name boundary, 2026-09-24 18:34:05" },
    { 1790274846LL, 0.4375001429075844, 12.919636843332619, 0.9619399380640226,
      "Full moon", 1791710032.254, 1790434310.815,
      "the second today's name turns full, 2026-09-24 18:34:06" },
    { 1790280000LL, 0.43952017637440416, 12.979289621110574, 0.9643312162973926,
      "Full moon", 1791710032.254, 1790434310.815,
      "today, 2026-09-24 20:00:00, 16:00 in Toronto" },
    { 1790434310LL, 0.49999968050012822, 14.765284991480648, 0.99999999999899258,
      "Full moon", 1791710032.254, 1790434310.815,
      "a second before the full moon after today, 2026-09-26 14:51:50" },
    { 1790434311LL, 0.5000000724352508, 14.765296565555609, 0.99999999999994826,
      "Full moon", 1791710032.254, 1792985753.692,
      "a second after the full moon after today, 2026-09-26 14:51:51" },
    { 1791710032LL, 0.99999990059145794, 29.530585917407215, 9.7533092713320002e-14,
      "New moon", 1791710032.254, 1792985753.692,
      "the second the new moon after today falls in, 2026-10-11 09:13:52" },
    { 1791710033LL, 2.9252651882183441e-07, 8.6384803559269585e-06, 8.4454665483235658e-13,
      "New moon", 1794261475.131, 1792985753.692,
      "a second after the new moon after today, 2026-10-11 09:13:53" },
    { 2004112800LL, 0.24809842676172181, 7.326492635776539, 0.4940261736172008,
      "First quarter", 2006031233.913, 2004755512.475,
      "2033-07-04 18:00:00" },
    { 2147483648LL, 0.4401650013582909, 12.998331682591875, 0.96507881671519113,
      "Full moon", 2148912035.020, 2147636313.581,
      "2038-01-19 03:14:08, a second past 32-bit time_t" },
    { 2240611199LL, 0.94012031808444385, 27.762306585703293, 0.034972742361897069,
      "New moon", 2240763978.588, 2242039700.026,
      "2040-12-31 23:59:59" },
};

#define N_CASES ((int)(sizeof s_cases / sizeof s_cases[0]))

static int near(double a, double b, double tol)
{
    return fabs(a - b) <= tol;
}

static void check_case(const moon_case_t *c)
{
    char what[160];
    moonphase_t m;
    moonphase_compute(c->t, &m);

    snprintf(what, sizeof what, "%s: phase %.9f", c->what, c->phase);
    expect(what, near(m.phase, c->phase, 1e-9));

    snprintf(what, sizeof what, "%s: age %.6f d", c->what, c->age);
    expect(what, near(m.age_days, c->age, 1e-9));

    snprintf(what, sizeof what, "%s: illuminated %.9f", c->what, c->illuminated);
    expect(what, near(m.illuminated, c->illuminated, 1e-9));

    snprintf(what, sizeof what, "%s: named \"%s\"", c->what, c->name);
    expect(what, m.name != NULL && strcmp(m.name, c->name) == 0);

    /* The expected instants are fractional; the answer is the whole second
       at or after, so it is never more than a second off either way. */
    snprintf(what, sizeof what, "%s: next new at %.0f", c->what, c->next_new);
    expect(what, near((double)m.next_new, c->next_new, 1.0)
                 && (double)m.next_new >= c->next_new);

    snprintf(what, sizeof what, "%s: next full at %.0f", c->what, c->next_full);
    expect(what, near((double)m.next_full, c->next_full, 1.0)
                 && (double)m.next_full >= c->next_full);
}

/*
 * What the next new and full moons must satisfy for any input, whatever the
 * table says: strictly after it, no more than a month ahead (a month plus
 * the rounding second, for an input sitting exactly on an event), landing on
 * phase 0 and 0.5, and the event is in the second returned -- so the second
 * before still looks forward to the same event, and the phase there has not
 * yet reached it.
 */
static void check_next(long long t)
{
    moonphase_t m, at, before;
    moonphase_compute(t, &m);

    expect_quiet("next new is strictly after", t, m.next_new > t);
    expect_quiet("next full is strictly after", t, m.next_full > t);
    expect_quiet("next new within a month", t,
                 (double)(m.next_new - t) <= LUNATION_SECS + 1.0);
    expect_quiet("next full within a month", t,
                 (double)(m.next_full - t) <= LUNATION_SECS + 1.0);

    /* A second is 3.9e-7 of a cycle; the rounding up puts the answer just
       past the event, never short of it. */
    moonphase_compute(m.next_new, &at);
    expect_quiet("phase ~0 at next new", t,
                 at.phase < 1e-6 || at.phase > 1.0 - 1e-12);
    expect_quiet("named new at next new", t, strcmp(at.name, "New moon") == 0);
    moonphase_compute(m.next_new - 1, &before);
    expect_quiet("second before next new still looks to it", t,
                 before.next_new == m.next_new);
    expect_quiet("phase just short of 1 the second before next new", t,
                 before.phase > 1.0 - 1e-6);

    moonphase_compute(m.next_full, &at);
    expect_quiet("phase ~0.5 at next full", t,
                 at.phase >= 0.5 - 1e-12 && at.phase < 0.5 + 1e-6);
    expect_quiet("named full at next full", t, strcmp(at.name, "Full moon") == 0);
    moonphase_compute(m.next_full - 1, &before);
    expect_quiet("second before next full still looks to it", t,
                 before.next_full == m.next_full);
    expect_quiet("phase just short of 0.5 the second before next full", t,
                 before.phase < 0.5 + 1e-12 && before.phase > 0.5 - 1e-6);

    /* The events alternate: whichever comes first, the other follows about
       half a month later, never both within the same half. */
    double gap = fabs((double)(m.next_new - m.next_full));
    expect_quiet("new and full half a month apart", t,
                 near(gap, LUNATION_SECS / 2.0, 2.0));
}

/* The fields agree with each other, for any input. */
static void check_ranges(long long t)
{
    moonphase_t m;
    moonphase_compute(t, &m);
    expect_quiet("phase in [0, 1)", t, m.phase >= 0.0 && m.phase < 1.0);
    expect_quiet("age in [0, month)", t,
                 m.age_days >= 0.0 && m.age_days < MOONPHASE_SYNODIC_DAYS);
    expect_quiet("phase is age over the month", t,
                 near(m.phase, m.age_days / MOONPHASE_SYNODIC_DAYS, 1e-15));
    expect_quiet("illuminated in [0, 1]", t,
                 m.illuminated >= 0.0 && m.illuminated <= 1.0);
    expect_quiet("name is the phase's name", t,
                 m.name == moonphase_name(m.phase));
}

int main(void)
{
    /* The rule on its own, before any almanac: each eighth of the cycle is
       named for the phase at its centre, and the names change a sixteenth
       either side of it. */
    static const char *const names[MOONPHASE_NAME_COUNT] = {
        "New moon", "Waxing crescent", "First quarter", "Waxing gibbous",
        "Full moon", "Waning gibbous", "Last quarter", "Waning crescent",
    };
    for (int k = 0; k < MOONPHASE_NAME_COUNT; k++) {
        char what[96];
        snprintf(what, sizeof what, "phase %d/8 is \"%s\"", k, names[k]);
        expect(what, strcmp(moonphase_name(k / 8.0), names[k]) == 0);
    }
    expect("1/16 rounds up to the crescent, as int(x + 0.5) does",
           strcmp(moonphase_name(1.0 / 16.0), "Waxing crescent") == 0);
    expect("just short of 1/16 is still new",
           strcmp(moonphase_name(1.0 / 16.0 - 1e-9), "New moon") == 0);
    expect("15/16 wraps round to new",
           strcmp(moonphase_name(15.0 / 16.0), "New moon") == 0);
    expect("just short of 15/16 is the waning crescent",
           strcmp(moonphase_name(15.0 / 16.0 - 1e-9), "Waning crescent") == 0);
    expect("a phase of 1.25 wraps to the first quarter",
           strcmp(moonphase_name(1.25), "First quarter") == 0);
    expect("a phase of -0.25 wraps to the last quarter",
           strcmp(moonphase_name(-0.25), "Last quarter") == 0);
    expect("NaN is named, not an out-of-range index",
           strcmp(moonphase_name(NAN), "New moon") == 0);

    /* Against almanac.py, instant by instant. */
    for (int i = 0; i < N_CASES; i++) check_case(&s_cases[i]);

    /* The time base. The name turned full at 18:34:06 UTC on 2026-09-24,
       which was 14:34:06 in Toronto. A caller that hands over the local
       clock as if it were UTC, as almanac.py's main() does, is asking about
       four hours earlier and still sees the gibbous, and goes on seeing it
       until 22:34:06 UTC. That is the !today disagreement moonphase.h
       describes; these pin which side is right, so nobody "fixes" it by
       shifting the C. */
    {
        const long long flip = 1790274846LL, edt = 4LL * 3600;
        moonphase_t utc, local_as_utc;
        moonphase_compute(flip, &utc);
        moonphase_compute(flip - edt, &local_as_utc);
        expect("full at 18:34:06 UTC, given UTC",
               strcmp(utc.name, "Full moon") == 0);
        expect("the same instant given as EDT local time names the gibbous",
               strcmp(local_as_utc.name, "Waxing gibbous") == 0);
    }

    /* The properties at the table's own instants, where the edge cases are,
       and at the reference exactly, where the next new moon is a whole month
       away and nothing sooner will do. */
    int before = failures;
    for (int i = 0; i < N_CASES; i++) {
        check_next(s_cases[i].t);
        check_ranges(s_cases[i].t);
    }
    expect("next new and full hold at every table instant", failures == before);

    moonphase_t ref;
    moonphase_compute(MOONPHASE_REF_UNIX, &ref);
    expect("at the reference, the next new moon is a month off, rounded up",
           ref.next_new == MOONPHASE_REF_UNIX + (long long)ceil(LUNATION_SECS));

    /* The reference is the one event that falls on a whole second, so it is
       where a last-bit wobble in the arithmetic would show: every second of
       the day before it must see the new moon at the reference itself, and
       every second of the day from it on must see the one a month later. */
    int wrong_before = 0, wrong_after = 0;
    for (long long t = MOONPHASE_REF_UNIX - 86400; t < MOONPHASE_REF_UNIX; t++) {
        moonphase_t m;
        moonphase_compute(t, &m);
        if (m.next_new != MOONPHASE_REF_UNIX) wrong_before++;
    }
    for (long long t = MOONPHASE_REF_UNIX; t < MOONPHASE_REF_UNIX + 86400; t++) {
        moonphase_t m;
        moonphase_compute(t, &m);
        if (m.next_new != ref.next_new) wrong_after++;
    }
    expect("every second of the day before the reference looks to it",
           wrong_before == 0);
    expect("every second of the day after the reference looks a month on",
           wrong_after == 0);

    /* Then a sweep from 1999 to 2041, on a step that is not a whole number
       of hours or days so it lands at every part of the cycle and every time
       of day. About eight thousand instants, fifteen or so in each month. */
    before = failures;
    int n = 0, wobbles = 0;
    long long prev_new = 0, prev_full = 0;
    for (long long t = 915148800LL; t < 2240611200LL + 86400LL * 365;
         t += 86400LL * 2 - 4321) {
        check_next(t);
        check_ranges(t);

        /* Until the event a previous instant looked forward to has come, a
           later instant must look forward to exactly the same second. */
        moonphase_t m;
        moonphase_compute(t, &m);
        if (t < prev_new && m.next_new != prev_new) wobbles++;
        if (t < prev_full && m.next_full != prev_full) wobbles++;
        prev_new = m.next_new;
        prev_full = m.next_full;
        n++;
    }
    char what[96];
    snprintf(what, sizeof what, "next new and full hold across %d instants, 1999-2041", n);
    expect(what, failures == before);
    expect("the same month gives the same next new and full second, from any instant in it",
           wobbles == 0);

    if (failures) {
        printf("\nFAIL %d\n", failures);
        return 1;
    }
    printf("\nPASS\n");
    return 0;
}
