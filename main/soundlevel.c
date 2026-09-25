#include "soundlevel.h"

#include <math.h>
#include <string.h>

/*
 * Two speeds of arithmetic live here. The per-sample path -- three biquads on
 * each mic and a sum of squares, sixteen thousand times a second -- is float
 * only, for the S3's single-precision FPU; a stray double there would be
 * done in software. Everything that happens per block (eight times a second)
 * or less is free to use double where precision earns it: the day's energy
 * sums, the filter design at init, the reports.
 */

/* Strict C11 has no M_PI. */
#define SL_PI 3.14159265358979323846

/* ---- the A-weighting ---------------------------------------------------- */

/*
 * IEC 61672-1 Annex E gives the analog A-weighting as
 *
 *                            K s^4
 *     A(s) = ---------------------------------------------
 *            (s + w1)^2 (s + w2) (s + w3) (s + w4)^2
 *
 * with wn = 2 pi fn, the four frequencies below, and K setting 0 dB at 1 kHz.
 * This is a digital version of it for fs = 16 kHz, in two parts.
 *
 * THE LOW PART, s^4 / ((s + w1)^2 (s + w2) (s + w3)): four zeros at DC and four
 * poles, all far below Nyquist. The bilinear transform with each pole
 * pre-warped to its own frequency: a pole at f goes to
 *
 *     z = (1 - t) / (1 + t),   t = tan(pi f / fs)
 *
 * and the zeros at s = 0 go to z = 1. Pre-warping puts each corner exactly at
 * its analog frequency. Here, honestly, it hardly matters: the highest of
 * these corners, 738 Hz, moves by 0.7 % without it, and the response by a few
 * hundredths of a dB either way (0.095 dB worst against the curve with it,
 * 0.115 without). The frequency squeeze is worst near Nyquist, and there this
 * part is flat, as it is in the analog, where it tends to 1. Where warping
 * would matter is f4, below, which cannot be pre-warped. As two biquads:
 * (1 - z^-1)^2 over the poles p1, p2 and (1 - z^-1)^2 over p1, p3. The double
 * pole at 20.6 Hz is split between the two sections on purpose. A double pole
 * in one float biquad splits by the square root of a2's rounding error,
 * sqrt(6e-8), which moves f1 by some 3 %; two distinct poles in a section
 * move only by the error over their distance apart.
 *
 * THE HIGH PART, the double pole at f4 = 12.2 kHz, is above Nyquist (8 kHz),
 * and the bilinear transform cannot map it well. tan(pi f4 / fs) is past its
 * asymptote, so it cannot be pre-warped; unwarped it lands at z = -0.41, with
 * the transform's zero at z = -1, and at 6.3 kHz reads 7.8 dB down where the
 * analog curve is 2.05 dB down -- outside class 2 on its own. Below 8 kHz all
 * the double pole contributes is a gentle droop: 0.9 dB at 4 kHz, 2.05 dB at
 * 6.3 kHz. So it is replaced by something with the same droop: one real zero
 * at z = -a. The zero's power gain relative to DC is
 *
 *     (1 + a^2 + 2 a cos w) / (1 + a)^2,
 *
 * and setting that equal to the double pole's (1 + (f/f4)^2)^-2 at fm, the top
 * of the band this meter claims, gives
 *
 *     (1 - T) a^2 + 2 (cos wm - T) a + (1 - T) = 0,   T = (1 + (fm/f4)^2)^-2,
 *
 * whose roots multiply to 1; the one inside the unit circle is the zero. It
 * tracks the double pole within 0.15 dB from DC to 6.3 kHz. The third section
 * is that zero, times the gain that makes the whole cascade 0 dB at 1 kHz.
 *
 * The numbers at 16 kHz, derived at init by aw_design() and repeated here
 * only for a reader:
 *
 *     p1 = 0.9919433   p2 = 0.9585940   p3 = 0.7453512
 *     T  = 0.623024 (-2.055 dB)   a = 0.136324   gain = 0.9383750
 *     section 1:  b = 1, -2, 1                 a1 = -1.9505373  a2 = 0.9508709
 *     section 2:  b = 1, -2, 1                 a1 = -1.7372946  a2 = 0.7393462
 *     section 3:  b = 0.9383750, 0.1279231, 0  a1 = 0           a2 = 0
 *
 * What that achieves against the exact analog curve: within 0.11 dB at every
 * IEC 61672-1 table point from 31.5 Hz to 6.3 kHz (the tests hold it to 0.15
 * dB, and to the standard's class 2 limits).
 *
 * ABOVE 6.3 kHz, honestly. Between 6.3 kHz and Nyquist the zero droops less
 * than the double pole it stands in for, so the filter reads high: +0.25 dB
 * at 7 kHz, +0.7 dB at 7.9 kHz. That is still inside class 2 at 8 kHz
 * (+-5.6 dB), but it is no longer the curve -- and little arrives there
 * anyway, since the ES7210's decimation filter passes only to 0.4535 fs, about
 * 7.3 kHz. Above 8 kHz nothing is measured at all. The standard still counts
 * 8-20 kHz, weighted -1.1 dB at 8 kHz down to -9.3 dB at 20 kHz, and that
 * energy is simply missing, so the meter reads LOW by however much of the
 * sound lived up there. For speech, traffic, music and most room noise that is
 * a small share of the A-weighted energy; for hiss -- running water, a hair
 * dryer, a kettle near the boil -- it can be a dB or more. Not measured. So
 * this is a class-2-shaped meter for ordinary rooms, not a class 2 meter.
 * Capture at 48 kHz, the spec's later step, would fix it: there f4 is below
 * Nyquist and becomes an ordinary pre-warped bilinear pair instead of the
 * zero, and the matching below has no solution (hence the assert).
 *
 * BELOW, the hardware takes a share first: at 24 dB of PGA the mic's coupling
 * caps against the ES7210's 6 kohm input make a high-pass around 53 Hz, and
 * the codec's own digital high-pass sits in front of that. A-weighting is
 * already -30 dB at 50 Hz, so it matters only for rumble-dominated noise, which
 * reads somewhat low. This filter cannot know it; calibration at 1 kHz does
 * not see it either.
 */
#define F1  20.598997       /* Hz, IEC 61672-1 Annex E */
#define F2  107.65265
#define F3  737.86223
#define F4  12194.217
#define FM  6300.0          /* where the zero is matched to the f4 droop */

_Static_assert(SOUNDLEVEL_FS == 16000,
               "the A-weighting's f4 zero is matched for 16 kHz; at 48 kHz map f4 as a pre-warped pair");

/* A pole at f Hz through the pre-warped bilinear transform. */
static double warped_pole(double f)
{
    double t = tan(SL_PI * f / SOUNDLEVEL_FS);
    return (1.0 - t) / (1.0 + t);
}

/* The zero standing in for the f4 double pole: the root inside the unit
   circle, written as 2A / (-B + sqrt(B^2 - 4A^2)) so that nothing cancels. */
static double f4_zero(void)
{
    double r = FM / F4;
    double tgt = 1.0 / ((1.0 + r * r) * (1.0 + r * r));
    double u = cos(2.0 * SL_PI * FM / SOUNDLEVEL_FS);
    double qa = 1.0 - tgt, qb = 2.0 * (u - tgt);
    return 2.0 * qa / (-qb + sqrt(qb * qb - 4.0 * qa * qa));
}

/* |B(e^jw) / A(e^jw)|^2 of one section, in double: used at init and by the
   tests, never per sample. */
static double section_gain2(double b0, double b1, double b2, double a1, double a2, double w)
{
    double c1 = cos(w), s1 = sin(w), c2 = cos(2.0 * w), s2 = sin(2.0 * w);
    double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
    double dr = 1.0 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
    return (nr * nr + ni * ni) / (dr * dr + di * di);
}

static void aw_design(soundlevel_biquad_t aw[3])
{
    double p1 = warped_pole(F1), p2 = warped_pole(F2), p3 = warped_pole(F3);
    double a = f4_zero();
    double c[3][5] = {
        /* b0,  b1,   b2,  a1,         a2 */
        { 1.0, -2.0, 1.0, -(p1 + p2), p1 * p2 },
        { 1.0, -2.0, 1.0, -(p1 + p3), p1 * p3 },
        { 1.0,  a,   0.0,  0.0,       0.0     },
    };
    double w = 2.0 * SL_PI * 1000.0 / SOUNDLEVEL_FS, g2 = 1.0;

    for (int i = 0; i < 3; i++) g2 *= section_gain2(c[i][0], c[i][1], c[i][2], c[i][3], c[i][4], w);
    c[2][0] /= sqrt(g2);
    c[2][1] /= sqrt(g2);

    for (int i = 0; i < 3; i++) {
        aw[i].b0 = (float)c[i][0];
        aw[i].b1 = (float)c[i][1];
        aw[i].b2 = (float)c[i][2];
        aw[i].a1 = (float)c[i][3];
        aw[i].a2 = (float)c[i][4];
        aw[i].z1 = aw[i].z2 = 0.0f;
    }
}

float soundlevel_aweight_db(const soundlevel_t *s, float f_hz)
{
    double w = 2.0 * SL_PI * (double)f_hz / SOUNDLEVEL_FS, g2 = 1.0;
    for (int i = 0; i < 3; i++) {
        const soundlevel_biquad_t *q = &s->aw[0][i];            /* the two chains are the same design */
        g2 *= section_gain2((double)q->b0, (double)q->b1, (double)q->b2, (double)q->a1, (double)q->a2, w);
    }
    return (float)(10.0 * log10(g2));
}

/* Transposed direct form II: two states, and in float it keeps its precision
   at the quiet end, where a quiet room is a few codes of the ADC. */
static inline float biquad(soundlevel_biquad_t *q, float x)
{
    float y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

/* ---- levels ------------------------------------------------------------- */

/*
 * dBFS here is AES17's: a full-scale SINE is 0 dBFS. With samples scaled to
 * +-1, a full-scale sine's mean square is 1/2, so a mean square m reads
 * 10 log10(2 m). The other habit -- 10 log10(m), full-scale square wave 0 dB,
 * full-scale sine -3.01 -- would do as well if it were used throughout; this
 * one is chosen because the ES7210 datasheet states its full scale as the rms
 * of a sine, which is what the offset below is derived from.
 */
#define AES17_DB  3.0103f

/* Digital silence has no level. Floor it at about -127 dBFS, below the
   histogram and far below the mic's own noise, so a block of zeros reads as
   very quiet rather than as minus infinity. */
#define MS_FLOOR  1e-13f

/* The filter starting from rest rings for a few tens of milliseconds, the
   slowest part (the double pole at 20.6 Hz) with a time constant near 8 ms.
   Two blocks, 250 ms, and it is gone. */
#define WARMUP_BLOCKS 2

#define SECS_PER_DAY  86400u

static float dbfs_of(float ms)
{
    return 10.0f * log10f(ms) + AES17_DB;
}

/* The energy mean of the last k included blocks, in dBFS; NAN if none. */
static float ring_dbfs(const soundlevel_t *s, int k)
{
    float sum = 0.0f;
    if (k > s->ring_n) k = s->ring_n;
    if (k <= 0) return NAN;
    for (int i = 1; i <= k; i++) sum += s->ring[(s->ring_head - i + SOUNDLEVEL_RING) % SOUNDLEVEL_RING];
    return dbfs_of(sum / (float)k);
}

/*
 * The estimated offset, SOUNDLEVEL_CAL_EST_DB = 114.0 dB, with dBFS as above:
 *
 *  - The ES7210's full scale is 2 AVDD / 3.3 Vrms differential (datasheet),
 *    2.0 Vrms at AVDD = 3.3 V: 0 dBFS is +6.02 dBV, the rms of a sine.
 *  - The mics are pseudo-differential. The capsule drives MIC1P through two
 *    1 uF in series; MIC1N is held at AC ground through its own pair. The ADC
 *    converts P - N, N stands still, so the whole capsule signal is the
 *    differential signal: the wiring neither loses nor gains 6 dB.
 *  - The PGA adds 24 dB, so at the capsule full scale is 6.02 - 24 = -17.98
 *    dBV.
 *  - A typical analog MEMS capsule gives -38 dBV at 1 Pa, 94 dB SPL.
 *  - So 94 dB SPL reads -38 + 17.98 = -20.02 dBFS: dB SPL = dBFS + 114.02,
 *    and at 1 kHz, where A-weighting is 0 dB, dBA the same.
 *
 * Why it is only good to +-6 dB until someone calibrates:
 *  - The capsule is unmarked and no source gives its sensitivity. Analog MEMS
 *    parts run from the low -30s to the mid -40s of dBV/Pa, each with a
 *    production spread of a dB or more; -38 is a middle, not a measurement.
 *  - The capsule's own output impedance, some hundreds of ohms, into the
 *    ES7210's 6 kohm costs a few tenths of a dB; the 0.5 uF of coupling
 *    costs 0.01 dB at 1 kHz. The PGA has its own tolerance.
 *  - Two mics need no correction: their energies are averaged (below), so a
 *    room that is 94 dB at both reads as 94 dB at one, whatever the sound's
 *    direction or spectrum. A mismatch between the two capsules' own
 *    sensitivities is a fixed offset like the rest, and `!cal` takes it out.
 *
 * Hence "est" in the log until `!cal` against a real meter -- beside the
 * board in the room, not a calibrator over one port (see soundlevel.h).
 */

static void acc_open(soundlevel_acc_t *a, int32_t day, uint32_t tod_s)
{
    memset(a, 0, sizeof *a);
    a->day = day;
    a->tod_s = tod_s;
    a->max_dbfs = -INFINITY;
    a->min_dbfs = INFINITY;
    a->l90_dbfs = NAN;
}

static uint32_t acc_blocks(const soundlevel_acc_t *a)
{
    uint32_t n = 0;
    for (int p = 0; p < SOUNDLEVEL_NPERIOD; p++) n += a->blocks[p];
    return n;
}

static void acc_add(soundlevel_acc_t *a, soundlevel_period_t p, float ms, float db, bool red)
{
    a->blocks[p]++;
    a->energy[p] += (double)ms;                 /* per block, not per sample: double is affordable */
    if (db > a->max_dbfs) a->max_dbfs = db;
    if (db < a->min_dbfs) a->min_dbfs = db;
    if (red) a->red_blocks++;
}

static int bin_of(float db)
{
    long i = lrintf((db - SOUNDLEVEL_HIST_LO_DBFS) / SOUNDLEVEL_HIST_STEP_DB);
    if (i < 0) i = 0;
    if (i > SOUNDLEVEL_BINS - 1) i = SOUNDLEVEL_BINS - 1;
    return (int)i;
}

/*
 * L90, the level exceeded 90 % of the time: walking down from the loudest
 * bin, the first whose count, with everything above it, reaches 90 % of the
 * blocks. So it is the highest level that at least 90 % of the blocks meet or
 * exceed -- with ten blocks on each 0.5 dB bin from 30 to 79.5, 35.0, not the
 * 34.5 that "the lowest 10 %" would say. Reported at the bin's centre, so to
 * +-0.25 dB.
 */
static float l90_of(const uint32_t *h)
{
    uint64_t total = 0, cum = 0;
    for (int i = 0; i < SOUNDLEVEL_BINS; i++) total += h[i];
    if (!total) return NAN;
    for (int i = SOUNDLEVEL_BINS - 1; i >= 0; i--) {
        cum += h[i];
        if (cum * 10u >= total * 9u) return SOUNDLEVEL_HIST_LO_DBFS + SOUNDLEVEL_HIST_STEP_DB * (float)i;
    }
    return SOUNDLEVEL_HIST_LO_DBFS;
}

soundlevel_period_t soundlevel_period(uint32_t tod_s)
{
    tod_s %= SECS_PER_DAY;
    if (tod_s >= 7u * 3600u && tod_s < 19u * 3600u) return SOUNDLEVEL_DAY;
    if (tod_s >= 19u * 3600u && tod_s < 23u * 3600u) return SOUNDLEVEL_EVENING;
    return SOUNDLEVEL_NIGHT;
}

/*
 * Close whatever interval this block's time has left behind. A day is a
 * calendar date, so its Lnight is that date's 00-07 plus its 23-24: the night
 * that crosses midnight is split between two dates, as a daily line has to
 * split it. The roll happens on the first counted block of the new second,
 * minute or date, which keeps it exact -- no block lands in the wrong one
 * however late the caller gets round to reading -- and means a closed
 * interval appears up to one block (125 ms) after its end.
 *
 * Only a date that differs counts as a new day, forwards or back: a clock set
 * back a day by the Mac closes today into `yesterday` too. Rare, harmless,
 * and better than mixing two dates.
 */
static void roll(soundlevel_t *s, int32_t day, uint32_t tod_s)
{
    uint32_t minute = tod_s - tod_s % 60u;

    if (s->day.day != day) {
        if (acc_blocks(&s->day)) {
            s->yesterday = s->day;
            s->yesterday.l90_dbfs = l90_of(s->day_hist);
        }
        acc_open(&s->day, day, 0);
        memset(s->day_hist, 0, sizeof s->day_hist);
    }
    if (s->minute.day != day || s->minute.tod_s != minute) {
        if (acc_blocks(&s->minute)) {
            s->last_minute = s->minute;
            s->last_minute.l90_dbfs = l90_of(s->minute_hist);
        }
        acc_open(&s->minute, day, minute);
        memset(s->minute_hist, 0, sizeof s->minute_hist);
    }
    if (s->second.day != day || s->second.tod_s != tod_s) {
        if (acc_blocks(&s->second)) s->last_second = s->second;
        acc_open(&s->second, day, tod_s);
    }
}

/*
 * One counted block. The ring and the fast level always; red against the
 * LAeq,3s that includes this block; then, if the clock is good, the second,
 * the minute and the day, each in the period this time of day falls in.
 */
static void take_block(soundlevel_t *s, float ms, int32_t day, uint32_t tod_s)
{
    float db;
    bool red;

    if (!(ms >= MS_FLOOR)) ms = MS_FLOOR;       /* zeros, and a NAN, read as the floor */
    db = dbfs_of(ms);

    s->ring[s->ring_head] = ms;
    s->ring_head = (s->ring_head + 1) % SOUNDLEVEL_RING;
    if (s->ring_n < SOUNDLEVEL_RING) s->ring_n++;
    s->laf_dbfs = db;

    red = ring_dbfs(s, SOUNDLEVEL_RING) + s->cal_offset >= SOUNDLEVEL_RED_DBA;
    s->red_run = red ? s->red_run + 1u : 0u;

    if (day < 0) return;
    if (tod_s >= SECS_PER_DAY) tod_s = SECS_PER_DAY - 1u;
    roll(s, day, tod_s);

    soundlevel_period_t p = soundlevel_period(tod_s);
    int bin = bin_of(db);
    acc_add(&s->second, p, ms, db, red);
    acc_add(&s->minute, p, ms, db, red);
    acc_add(&s->day, p, ms, db, red);
    s->minute_hist[bin]++;
    s->day_hist[bin]++;
}

void soundlevel_init(soundlevel_t *s, float cal_offset_db, bool calibrated)
{
    memset(s, 0, sizeof *s);
    aw_design(s->aw[0]);
    memcpy(s->aw[1], s->aw[0], sizeof s->aw[1]);
    s->warmup = WARMUP_BLOCKS;
    s->laf_dbfs = NAN;
    s->cal_offset = isfinite(cal_offset_db) ? cal_offset_db : SOUNDLEVEL_CAL_EST_DB;
    s->calibrated = calibrated && isfinite(cal_offset_db);
    acc_open(&s->second, SOUNDLEVEL_NO_DAY, 0);
    acc_open(&s->last_second, SOUNDLEVEL_NO_DAY, 0);
    acc_open(&s->minute, SOUNDLEVEL_NO_DAY, 0);
    acc_open(&s->last_minute, SOUNDLEVEL_NO_DAY, 0);
    acc_open(&s->day, SOUNDLEVEL_NO_DAY, 0);
    acc_open(&s->yesterday, SOUNDLEVEL_NO_DAY, 0);
}

/*
 * TWO MICS, ONE LEVEL. Each mic has its own A-weighting, and a block's mean
 * square is the mean of the two mics' mean squares: the energy mean, not the
 * energy of the mean signal. Averaging the samples first looks the same and
 * is not. The capsules are 34.5 mm apart (the board's STEP model), and the
 * mean of two signals is as loud as either only when they arrive in phase.
 * Room sound does not, just where A-weighting cares most. In a diffuse field
 * the two decorrelate as sinc(kd): the sample mean read 0.6 dB low at 2 kHz
 * and 2.1 at 4 kHz. Sound from along the line of the mics arrives 101 us
 * apart: 1.9 dB low at 2 kHz, 10.4 at 4 kHz. Broadband, A-weighted, that is
 * about 0.5 dB for speech and 1.8 for hiss in a diffuse room, and up to 4.6
 * from end-on -- a bias that moves with spectrum and direction, which no one
 * `!cal` can take out. The energy mean has none: each mic reads the level
 * where it is, and the mean of the two is the room's level at the board,
 * whatever the field.
 *
 * What it costs. The sample mean also halved the capsules' self-noise power
 * (uncorrelated, it averaged down 3 dB), and the energy mean does not, so the
 * quietest the meter can read is one capsule's floor, 3 dB above where the
 * sample mean's was. And six biquads a frame instead of three, 96,000 a
 * second: nothing, on the S3's FPU.
 *
 * The block's sum of squares is float: four thousand positive terms lose at
 * most two parts in ten thousand, 0.001 dB.
 *
 * The gate is read per block, not per sample: `tainted` is set the moment the
 * gate goes on and is cleared only when a block ends with the gate off, so a
 * block the gate touched for a single sample is dropped whole. The ring keeps
 * the last 24 COUNTED blocks, so across a chime LAeq,3s spans a little more
 * than three seconds of wall time -- the three seconds either side of it.
 */
int soundlevel_feed(soundlevel_t *s, const int16_t *mic1, const int16_t *mic2, size_t stride, size_t n,
                    int32_t day, uint32_t tod_s)
{
    int counted = 0;

    for (size_t i = 0; i < n; i++) {
        float a = (float)mic1[i * stride] * (1.0f / 32768.0f);
        float b = (float)mic2[i * stride] * (1.0f / 32768.0f);
        a = biquad(&s->aw[0][0], a);
        a = biquad(&s->aw[0][1], a);
        a = biquad(&s->aw[0][2], a);
        b = biquad(&s->aw[1][0], b);
        b = biquad(&s->aw[1][1], b);
        b = biquad(&s->aw[1][2], b);
        s->sum += a * a + b * b;
        if (++s->n < SOUNDLEVEL_BLOCK) continue;

        float ms = s->sum / (float)(2 * SOUNDLEVEL_BLOCK);
        bool drop = s->tainted || s->warmup > 0;
        s->sum = 0.0f;
        s->n = 0;
        s->tainted = s->exclude;
        if (s->warmup > 0) s->warmup--;
        if (!drop) {
            take_block(s, ms, day, tod_s);
            counted++;
        }
    }
    return counted;
}

void soundlevel_exclude(soundlevel_t *s, bool on)
{
    s->exclude = on;
    if (on) s->tainted = true;
}

bool soundlevel_block(soundlevel_t *s, float mean_square, int32_t day, uint32_t tod_s)
{
    if (s->exclude) return false;
    take_block(s, mean_square, day, tod_s);
    return true;
}

void soundlevel_now(const soundlevel_t *s, soundlevel_now_t *out)
{
    out->have = s->ring_n > 0;
    out->laf_dbfs = out->have ? s->laf_dbfs : NAN;
    out->laeq1s_dbfs = ring_dbfs(s, SOUNDLEVEL_BLOCKS_1S);
    out->laeq3s_dbfs = ring_dbfs(s, SOUNDLEVEL_RING);
    out->laf = out->laf_dbfs + s->cal_offset;
    out->laeq1s = out->laeq1s_dbfs + s->cal_offset;
    out->laeq3s = out->laeq3s_dbfs + s->cal_offset;
    out->red_run_s = (float)s->red_run / (float)SOUNDLEVEL_BLOCKS_1S;
    out->cal_offset = s->cal_offset;
    out->calibrated = s->calibrated;
}

/* ---- reports ------------------------------------------------------------ */

static float level_of(double energy, uint32_t n, float off)
{
    if (!n) return NAN;
    return (float)(10.0 * log10(energy / (double)n)) + AES17_DB + off;
}

static void report(const soundlevel_acc_t *a, float l90_dbfs, float off, soundlevel_report_t *out)
{
    double e = 0.0;
    uint32_t n = 0;

    memset(out, 0, sizeof *out);
    out->day = a->day;
    out->tod_s = a->tod_s;
    for (int p = 0; p < SOUNDLEVEL_NPERIOD; p++) {
        out->period[p] = level_of(a->energy[p], a->blocks[p], off);
        e += a->energy[p];
        n += a->blocks[p];
    }
    out->blocks = n;
    out->laeq = level_of(e, n, off);
    out->lmax = n ? a->max_dbfs + off : NAN;
    out->lmin = n ? a->min_dbfs + off : NAN;
    out->l90 = l90_dbfs + off;
    out->red_s = (float)a->red_blocks / (float)SOUNDLEVEL_BLOCKS_1S;
}

bool soundlevel_last_second(const soundlevel_t *s, soundlevel_report_t *out)
{
    if (!acc_blocks(&s->last_second)) return false;
    report(&s->last_second, NAN, s->cal_offset, out);
    return true;
}

bool soundlevel_last_minute(const soundlevel_t *s, soundlevel_report_t *out)
{
    if (!acc_blocks(&s->last_minute)) return false;
    report(&s->last_minute, s->last_minute.l90_dbfs, s->cal_offset, out);
    return true;
}

bool soundlevel_today(const soundlevel_t *s, soundlevel_report_t *out)
{
    if (!acc_blocks(&s->day)) return false;
    report(&s->day, l90_of(s->day_hist), s->cal_offset, out);
    return true;
}

bool soundlevel_yesterday(const soundlevel_t *s, soundlevel_report_t *out)
{
    if (!acc_blocks(&s->yesterday)) return false;
    report(&s->yesterday, s->yesterday.l90_dbfs, s->cal_offset, out);
    return true;
}

/*
 * Against LAeq,3s rather than LAF: whoever types `!cal 52` has read a meter
 * that averages too, and a room is steadier over three seconds than over one
 * eighth. The window 10-130 dBA is every room this could be in, from a
 * studio to a jackhammer; outside it the number is a typo ("!cal 5"), and a
 * typo must not quietly shift every figure by 45 dB.
 */
float soundlevel_calibrate(soundlevel_t *s, float measured_now_dba)
{
    float leq = ring_dbfs(s, SOUNDLEVEL_RING);

    if (isnan(leq) || !(measured_now_dba >= 10.0f && measured_now_dba <= 130.0f)) return s->cal_offset;
    s->cal_offset = measured_now_dba - leq;
    s->calibrated = true;
    return s->cal_offset;
}

/* ---- today.bin ---------------------------------------------------------- */

/*
 * The day's totals, byte for byte, little-endian whatever the host, so the
 * file means the same to the host tests as to the board:
 *
 *      0  magic "SLD1" -- the '1' is the layout's version
 *      4  day, int32
 *      8  energy[3], IEEE double bits
 *     32  blocks[3], uint32
 *     44  max_dbfs, min_dbfs, IEEE float bits
 *     52  red_blocks, uint32
 *     56  histogram, 241 x uint32
 *   1020  CRC-32 (IEEE 802.3) of bytes 0-1019
 *
 * The histogram is the bulk of it and has to be there: without it L90 could
 * not survive a reboot. Everything is in dBFS, so a save stays true across a
 * recalibration.
 */
#define SAVE_MAGIC  0x31444C53u     /* "SLD1" as little-endian bytes */
#define OFF_DAY     4
#define OFF_ENERGY  8
#define OFF_BLOCKS  32
#define OFF_MAX     44
#define OFF_MIN     48
#define OFF_RED     52
#define OFF_HIST    56
#define OFF_CRC     (OFF_HIST + 4 * SOUNDLEVEL_BINS)

_Static_assert(OFF_CRC + 4 == SOUNDLEVEL_SAVE_BYTES, "today.bin layout and SOUNDLEVEL_SAVE_BYTES disagree");

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_f(uint8_t *p, float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    put32(p, u);
}

static float get_f(const uint8_t *p)
{
    uint32_t u = get32(p);
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static void put_d(uint8_t *p, double d)
{
    uint64_t u;
    memcpy(&u, &d, sizeof u);
    put32(p, (uint32_t)u);
    put32(p + 4, (uint32_t)(u >> 32));
}

static double get_d(const uint8_t *p)
{
    uint64_t u = (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
    double d;
    memcpy(&d, &u, sizeof d);
    return d;
}

/* Bit by bit: a kilobyte once a minute does not need a table. */
static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

size_t soundlevel_save_day(const soundlevel_t *s, uint8_t *buf, size_t cap)
{
    if (!buf || cap < SOUNDLEVEL_SAVE_BYTES) return 0;

    put32(buf, SAVE_MAGIC);
    put32(buf + OFF_DAY, (uint32_t)s->day.day);
    for (int p = 0; p < SOUNDLEVEL_NPERIOD; p++) {
        put_d(buf + OFF_ENERGY + 8 * p, s->day.energy[p]);
        put32(buf + OFF_BLOCKS + 4 * p, s->day.blocks[p]);
    }
    put_f(buf + OFF_MAX, s->day.max_dbfs);
    put_f(buf + OFF_MIN, s->day.min_dbfs);
    put32(buf + OFF_RED, s->day.red_blocks);
    for (int i = 0; i < SOUNDLEVEL_BINS; i++) put32(buf + OFF_HIST + 4 * i, s->day_hist[i]);
    put32(buf + OFF_CRC, crc32_of(buf, OFF_CRC));
    return SOUNDLEVEL_SAVE_BYTES;
}

bool soundlevel_restore_day(soundlevel_t *s, const uint8_t *buf, size_t len)
{
    int32_t day;

    if (!buf || len != SOUNDLEVEL_SAVE_BYTES) return false;
    if (get32(buf) != SAVE_MAGIC) return false;
    if (get32(buf + OFF_CRC) != crc32_of(buf, OFF_CRC)) return false;
    for (int p = 0; p < SOUNDLEVEL_NPERIOD; p++) {
        double e = get_d(buf + OFF_ENERGY + 8 * p);
        if (!(e >= 0.0) || isinf(e)) return false;      /* a good CRC over a bad writer */
    }

    day = (int32_t)get32(buf + OFF_DAY);
    if (acc_blocks(&s->day)) {
        if (s->day.day != day) return false;
    } else {
        acc_open(&s->day, day, 0);
        memset(s->day_hist, 0, sizeof s->day_hist);
    }

    for (int p = 0; p < SOUNDLEVEL_NPERIOD; p++) {
        s->day.energy[p] += get_d(buf + OFF_ENERGY + 8 * p);
        s->day.blocks[p] += get32(buf + OFF_BLOCKS + 4 * p);
    }
    s->day.max_dbfs = fmaxf(s->day.max_dbfs, get_f(buf + OFF_MAX));
    s->day.min_dbfs = fminf(s->day.min_dbfs, get_f(buf + OFF_MIN));
    s->day.red_blocks += get32(buf + OFF_RED);
    for (int i = 0; i < SOUNDLEVEL_BINS; i++) s->day_hist[i] += get32(buf + OFF_HIST + 4 * i);
    return true;
}
