#ifndef MOONPHASE_H
#define MOONPHASE_H

#include <stdint.h>

/*
 * The moon, from the time alone: how far through its cycle it is, what that
 * phase is called, how much of the disc is lit, and when the next new and
 * full moons fall. For watch's moon-phase aperture and its moon page.
 *
 * The model is deliberately the one in tools/almanac.py, number for number:
 * a single reference new moon and a mean synodic month, nothing else. The
 * real moon runs ahead of and behind that mean by up to about half a day, so
 * a phase name can change that much early or late against the sky; a drawn
 * disc cannot show the difference.
 *
 * Sharing the model makes the board and the Mac's !today line agree only
 * when both are given the same instant. almanac.py's moon() given a naive
 * UTC datetime matches this to the last bit, because its NEW_MOON is the
 * same UTC instant as MOONPHASE_REF_UNIX. But its main() passes
 * datetime.now(), which is naive local time, so !today runs behind this by
 * the UTC offset: 4 h in Toronto summer, 5 h in winter. Near each of the
 * eight name boundaries in a month the two then print different names for
 * that long, some 5% of the time; on 2026-09-24 the name turned "Full
 * moon" here at 18:34:06 UTC and on !today not until 22:34:06 UTC. The fix
 * belongs in almanac.py, which should call
 * moon(dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)) and keep the
 * local date for its day and holiday lines. Nothing here should shift by a
 * UTC offset to meet it half way: that would make this wrong against the
 * sky and still wrong against almanac.py once almanac.py is fixed.
 *
 * Everything is in double. Twenty-six years is about 8.2e8 seconds, and a
 * float's 24-bit mantissa holds that to the nearest minute or so; the next
 * new moon would then land on the wrong second and, near a boundary, the
 * phase name would flip a minute early or late. The S3's FPU is single
 * precision, so this is software double, but it runs at most once a second
 * and costs microseconds.
 *
 * Pure: no clock, no hardware, no allocation, no state. The caller brings a
 * Unix time in UTC. The board keeps local time, so the UTC offset has to come
 * off first; a few hours' error moves the phase by well under a hundredth,
 * but near a boundary it is the !today disagreement above, and it can move
 * the printed date of the next full moon by a day.
 */

/* The reference new moon, 2000-01-06 18:14:00 UTC, as almanac.py's NEW_MOON. */
#define MOONPHASE_REF_UNIX       947182440LL

/* The mean synodic month in days, as almanac.py's SYNODIC. */
#define MOONPHASE_SYNODIC_DAYS   29.530588853

/* The eight names, as almanac.py's PHASES. */
#define MOONPHASE_NAME_COUNT     8

typedef struct {
    /* 0 up to (not including) 1: 0 new, 0.25 first quarter, 0.5 full,
       0.75 last quarter. Below 0.5 it is waxing, lit on the right as seen
       from the northern hemisphere. */
    double phase;

    /* Days since the last new moon, 0 up to MOONPHASE_SYNODIC_DAYS. */
    double age_days;

    /* Fraction of the disc that is lit, 0..1: (1 - cos(2 pi phase)) / 2.
       Half at the quarters, which is what the eye sees, rather than the
       phase's own 0.25 and 0.75. */
    double illuminated;

    /* "Waxing gibbous" and so on; one of the eight, never NULL. */
    const char *name;

    /* Unix times (UTC) of the next new and next full moon, each strictly
       after the time asked about and no more than a synodic month (plus the
       rounding second) ahead. The model's instants fall on fractions of a
       second; these are the first whole second at or after the instant, so
       the phase at next_new is already a hair past 0 and the phase at
       next_full a hair past 0.5, never just short of them. */
    int64_t next_new;
    int64_t next_full;
} moonphase_t;

/* Everything above for one moment. */
void moonphase_compute(int64_t unix_utc, moonphase_t *out);

/* The name of any phase, by almanac.py's rule: the nearest of the eight,
   PHASES[int(phase * 8 + 0.5) % 8]. So "New moon" covers the last and first
   sixteenth of the cycle and "Full moon" the sixteenth either side of 0.5.
   A phase outside 0..1 is wrapped into it first. */
const char *moonphase_name(double phase);

#endif /* MOONPHASE_H */
