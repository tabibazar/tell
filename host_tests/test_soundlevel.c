#include "soundlevel.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Strict C11 has no M_PI. */
#define PI 3.14159265358979323846

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

static void expect_near(const char *what, double got, double want, double tol)
{
    int ok = fabs(got - want) <= tol;
    printf("%s %s: %.4f (want %.4f +- %.3f)\n", ok ? "ok  " : "FAIL", what, got, want, tol);
    if (!ok) failures++;
}

/* Static, as the firmware's will be, and every test starts it from
   soundlevel_init rather than trusting what the last one left. */
static soundlevel_t s, t;

/*
 * The dBFS convention under test: AES17, a full-scale SINE is 0 dBFS. With
 * samples scaled to +-1 a full-scale sine has mean square 1/2, so a level L
 * dBFS is a mean square of 0.5 * 10^(L/10). The other convention (full-scale
 * square wave = 0 dB, so a full-scale sine is -3.01) would put every level
 * here 3.01 dB lower, and the offset derivation 3.01 dB higher.
 */
static float ms_of_dbfs(double dbfs)
{
    return (float)(0.5 * pow(10.0, dbfs / 10.0));
}

/* The offset the block-level tests run with: dBA = dBFS + 100. Round, so a
   wanted dBA is easy to read off; not the estimate, so nothing passes by
   accident of the default. */
#define OFF 100.0f

static void blocks_at(soundlevel_t *m, double dba, int n, int32_t day, uint32_t tod_s)
{
    for (int i = 0; i < n; i++) soundlevel_block(m, ms_of_dbfs(dba - OFF), day, tod_s);
}

#define DAY0   20356                    /* 2025-09-25, days since 1970; any date would do */
#define HMS(h, m, sec)  ((uint32_t)((h) * 3600 + (m) * 60 + (sec)))

/* ---- samples ------------------------------------------------------------ */

/* A sample as the codec would give it: rounded to int16, and clipped. */
static int16_t q16(double v)
{
    if (v > 32767.0) v = 32767.0;
    if (v < -32768.0) v = -32768.0;
    return (int16_t)lrint(v);
}

/* A sine at `f` Hz, peak `amp` as a fraction of full scale (32768), fed in
   chunks of `chunk` samples from sample index *pos on. The same sound at
   both mics, in phase: one array, handed over as MIC1 and as MIC2. */
static void feed_sine(soundlevel_t *m, double f, double amp, long samples, long *pos, size_t chunk,
                      int32_t day, uint32_t tod_s)
{
    static int16_t buf[4096];
    while (samples > 0) {
        size_t k = chunk < (size_t)samples ? chunk : (size_t)samples;
        if (k > sizeof buf / sizeof buf[0]) k = sizeof buf / sizeof buf[0];
        for (size_t i = 0; i < k; i++)
            buf[i] = q16(amp * 32768.0 * sin(2.0 * PI * f * (double)(*pos + (long)i) / SOUNDLEVEL_FS));
        soundlevel_feed(m, buf, buf, 1, k, day, tod_s);
        *pos += (long)k;
        samples -= (long)k;
    }
}

/*
 * The two mics as the app will hand them over: 4-slot TDM frames laid out
 * [ref, MIC1, unused, MIC2], the driver's halved-bits read in
 * docs/hardware/speaker-pinout.md. MIC1 is a sine of peak amp1 and MIC2 the
 * same frequency at amp2, lagging it by `lag` radians. The other two slots
 * carry a full-scale 8 kHz square, which the weighting passes, so a stride
 * that is off by a slot reads near 0 dBFS instead of the tone.
 */
static void feed_pair(soundlevel_t *m, double f, double amp1, double amp2, double lag,
                      long samples, long *pos, size_t chunk, int32_t day, uint32_t tod_s)
{
    static int16_t tdm[4 * 1024];
    while (samples > 0) {
        size_t k = chunk < (size_t)samples ? chunk : (size_t)samples;
        if (k > 1024) k = 1024;
        for (size_t i = 0; i < k; i++) {
            double ph = 2.0 * PI * f * (double)(*pos + (long)i) / SOUNDLEVEL_FS;
            int16_t junk = ((*pos + (long)i) & 1) ? 30000 : -30000;
            tdm[4 * i + 0] = junk;
            tdm[4 * i + 1] = q16(amp1 * 32768.0 * sin(ph));
            tdm[4 * i + 2] = (int16_t)-junk;
            tdm[4 * i + 3] = q16(amp2 * 32768.0 * sin(ph - lag));
        }
        soundlevel_feed(m, tdm + 1, tdm + 3, 4, k, day, tod_s);
        *pos += (long)k;
        samples -= (long)k;
    }
}

/* ---- the A-weighting ---------------------------------------------------- */

/* IEC 61672-1 Annex E: the analog A-weighting, normalised to 0 dB at 1 kHz,
   from the four pole frequencies. Computed here in double, independently of
   the module, as the reference the digital design is held to. */
static double analog_a_db(double f)
{
    const double f1 = 20.598997, f2 = 107.65265, f3 = 737.86223, f4 = 12194.217;
    double ff = f * f;
    double num = f4 * f4 * ff * ff;
    double den = (ff + f1 * f1) * sqrt((ff + f2 * f2) * (ff + f3 * f3)) * (ff + f4 * f4);
    return 20.0 * log10(num / den);
}

static double analog_a_rel_db(double f)
{
    return analog_a_db(f) - analog_a_db(1000.0);
}

/*
 * IEC 61672-1 Table 3: the nominal A-weighting at the octave points the task
 * names, and the class 2 acceptance limits there (symmetric at these points).
 */
static const struct { double f, a_db, class2; } iec[] = {
    {   31.5, -39.4, 3.5 },
    {   63.0, -26.2, 2.5 },
    {  125.0, -16.1, 2.0 },
    {  250.0,  -8.6, 1.9 },
    {  500.0,  -3.2, 1.9 },
    { 1000.0,   0.0, 1.4 },
    { 2000.0,   1.2, 2.6 },
    { 4000.0,   1.0, 3.6 },
    { 6300.0,  -0.1, 5.1 },
};
#define N_IEC (sizeof iec / sizeof iec[0])

static void test_design_against_iec(void)
{
    char what[96];
    soundlevel_init(&s, OFF, true);

    expect_near("filter is 0 dB at 1 kHz", soundlevel_aweight_db(&s, 1000.0f), 0.0, 0.001);

    for (size_t i = 0; i < N_IEC; i++) {
        double got = soundlevel_aweight_db(&s, (float)iec[i].f);
        snprintf(what, sizeof what, "%.1f Hz within class 2 of IEC table", iec[i].f);
        expect_near(what, got, iec[i].a_db, iec[i].class2);
        /* Class 2 is loose -- several dB at the ends -- so a filter with a
           section wrong could still pass it. Hold the design to the exact
           analog curve as well; the design sheet says it is inside 0.11 dB. */
        snprintf(what, sizeof what, "%.1f Hz within 0.15 dB of the analog curve", iec[i].f);
        expect_near(what, got, analog_a_rel_db(iec[i].f), 0.15);
    }
}

/*
 * The same response measured, not evaluated: a sine through the per-sample
 * path, read as LAeq,3s. Three seconds hold a whole number of half-cycles at
 * every frequency in the table -- 31.5 Hz is 94.5 cycles, 189 periods of the
 * squared sine -- so the mean square is exact with no window leakage. Four
 * seconds are fed and the ring holds the last three, clear of the start.
 */
static void test_design_in_time(void)
{
    char what[96];
    const double amp = 0.25, level = 20.0 * log10(amp);      /* -12.04 dBFS */

    for (size_t i = 0; i < N_IEC; i++) {
        long pos = 0;
        soundlevel_now_t now;
        soundlevel_init(&s, OFF, true);
        feed_sine(&s, iec[i].f, amp, 4L * SOUNDLEVEL_FS, &pos, 512, DAY0, HMS(12, 0, 0));
        soundlevel_now(&s, &now);
        snprintf(what, sizeof what, "%.1f Hz sine through the filter matches its response", iec[i].f);
        expect_near(what, now.laeq3s_dbfs, level + soundlevel_aweight_db(&s, (float)iec[i].f), 0.05);
    }
}

/*
 * Known amplitude in, exact dBFS out. At 1 kHz the weighting is 0 dB, so a
 * sine of peak A reads 20 log10(A) dBFS on every block once the filter has
 * settled -- including a full-scale one, which is 0.00 here and would be -3.01
 * under the other convention. And at -70 dBFS, about ten codes of peak, where
 * a quiet room sits: float must not have lost it.
 */
static void test_dbfs_of_sine(void)
{
    static const double levels[] = { 0.0, -20.0, -70.0 };
    char what[96];

    for (size_t k = 0; k < sizeof levels / sizeof levels[0]; k++) {
        double amp = levels[k] == 0.0 ? 32767.0 / 32768.0 : pow(10.0, levels[k] / 20.0);
        double want = 20.0 * log10(amp);
        double worst = 0.0;
        long pos = 0;
        soundlevel_now_t now;

        soundlevel_init(&s, OFF, true);
        /* Past the two dropped warm-up blocks, then block by block. */
        feed_sine(&s, 1000.0, amp, 3L * SOUNDLEVEL_BLOCK, &pos, SOUNDLEVEL_BLOCK, DAY0, HMS(12, 0, 0));
        for (int b = 0; b < 16; b++) {
            feed_sine(&s, 1000.0, amp, SOUNDLEVEL_BLOCK, &pos, SOUNDLEVEL_BLOCK, DAY0, HMS(12, 0, 0));
            soundlevel_now(&s, &now);
            if (fabs(now.laf_dbfs - want) > fabs(worst)) worst = now.laf_dbfs - want;
        }
        snprintf(what, sizeof what, "1 kHz at %.0f dBFS: every block's LAF (worst error)", levels[k]);
        expect_near(what, want + worst, want, levels[k] < -60.0 ? 0.1 : 0.01);
        snprintf(what, sizeof what, "1 kHz at %.0f dBFS: LAF in dBA is dBFS + offset", levels[k]);
        expect_near(what, now.laf, now.laf_dbfs + OFF, 1e-4);
    }
}

/* The textbook figure: 100 Hz sits 19.1 dB down on the A curve. */
static void test_100hz_reads_low(void)
{
    soundlevel_now_t now;
    long pos = 0;
    double at_1k, at_100;

    soundlevel_init(&s, OFF, true);
    feed_sine(&s, 1000.0, 0.5, 4L * SOUNDLEVEL_FS, &pos, 700, DAY0, HMS(12, 0, 0));
    soundlevel_now(&s, &now);
    at_1k = now.laeq3s;

    soundlevel_init(&s, OFF, true);
    pos = 0;
    feed_sine(&s, 100.0, 0.5, 4L * SOUNDLEVEL_FS, &pos, 700, DAY0, HMS(12, 0, 0));
    soundlevel_now(&s, &now);
    at_100 = now.laeq3s;

    expect_near("100 Hz reads 19.1 dB under 1 kHz of the same amplitude", at_100 - at_1k, -19.1, 0.2);
}

/* A block that is split across calls is the same block. */
static void test_feed_any_n(void)
{
    soundlevel_now_t a, b;
    long pa = 0, pb = 0;

    soundlevel_init(&s, OFF, true);
    soundlevel_init(&t, OFF, true);
    feed_sine(&s, 440.0, 0.3, 2L * SOUNDLEVEL_FS, &pa, 4096, DAY0, HMS(12, 0, 0));
    feed_sine(&t, 440.0, 0.3, 2L * SOUNDLEVEL_FS, &pb, 37, DAY0, HMS(12, 0, 0));
    soundlevel_now(&s, &a);
    soundlevel_now(&t, &b);
    expect("chunks of 37 and of 4096 give the same LAF, bit for bit", a.laf_dbfs == b.laf_dbfs);
    expect("... and the same LAeq,3s", a.laeq3s_dbfs == b.laeq3s_dbfs);
    expect("... and 14 blocks counted (16 less the 2 warm-up)", a.have && s.day.blocks[SOUNDLEVEL_DAY] == 14);
}

/* ---- two mics ----------------------------------------------------------- */

/*
 * The capsules sit 34.5 mm apart (the board's STEP model: MIC1 at x = 36.46
 * mm, MIC2 at 1.98). A sound arriving along the line through them reaches
 * the far one d/c = 101 us late, 2.53 rad of phase at 4 kHz. The mean of the
 * two SAMPLES is then cos^2(1.26) = 10.4 dB under what either mic hears;
 * the mean of the two ENERGIES is what either hears. Each case below is
 * expected to read as one mic would: the tone's level through the weighting.
 */
#define MIC_GAP_M   0.0345
#define SOUND_M_S   343.0

static float tone_via_pair(double f, double amp1, double amp2, double lag)
{
    soundlevel_now_t now;
    long pos = 0;
    soundlevel_init(&s, OFF, true);
    feed_pair(&s, f, amp1, amp2, lag, 4L * SOUNDLEVEL_FS, &pos, 700, DAY0, HMS(12, 0, 0));
    soundlevel_now(&s, &now);
    return now.laeq3s_dbfs;
}

static void test_two_mics_tone(void)
{
    const double amp = 0.25, level = 20.0 * log10(amp);        /* -12.04 dBFS */
    soundlevel_now_t one, two;
    char what[112];
    long pa = 0, pb = 0;

    /* The same sound at both, in phase: one array handed over twice is the
       TDM pair, block for block, and reads the tone. */
    soundlevel_init(&s, OFF, true);
    soundlevel_init(&t, OFF, true);
    feed_sine(&s, 1000.0, amp, 4L * SOUNDLEVEL_FS, &pa, 700, DAY0, HMS(12, 0, 0));
    feed_pair(&t, 1000.0, amp, amp, 0.0, 4L * SOUNDLEVEL_FS, &pb, 700, DAY0, HMS(12, 0, 0));
    soundlevel_now(&s, &one);
    soundlevel_now(&t, &two);
    expect("one array as both mics reads the TDM pair's level, bit for bit",
           one.laf_dbfs == two.laf_dbfs && one.laeq3s_dbfs == two.laeq3s_dbfs);
    expect_near("in phase at both mics: the tone", two.laeq3s_dbfs, level, 0.02);

    /* Along the mic axis, where the sample mean lost 0.4 dB at 1 kHz and
       10.4 dB at 4 kHz. */
    static const double freqs[] = { 1000.0, 2000.0, 4000.0, 6300.0 };
    for (size_t i = 0; i < sizeof freqs / sizeof freqs[0]; i++) {
        double lag = 2.0 * PI * freqs[i] * MIC_GAP_M / SOUND_M_S;
        snprintf(what, sizeof what, "%.0f Hz arriving along the mic axis reads as one mic does", freqs[i]);
        expect_near(what, tone_via_pair(freqs[i], amp, amp, lag),
                    level + soundlevel_aweight_db(&s, (float)freqs[i]), 0.05);
    }

    /* Opposite phase: the samples cancel, the energies do not. */
    expect_near("the two mics in anti-phase read the tone, not silence",
                tone_via_pair(1000.0, amp, amp, PI), level, 0.05);

    /*
     * One mic alone, the other silent: half the energy, 3.01 dB under the
     * tone. This is a calibrator sealed over one port, and why soundlevel.h
     * says `!cal 91`, not 94, for one. The sample mean read 6.02 under.
     */
    expect_near("a tone at MIC1 alone reads 3.01 dB under it (a one-port calibrator)",
                tone_via_pair(1000.0, amp, 0.0, 0.0), level - 3.0103, 0.02);
    expect_near("... and at MIC2 alone the same",
                tone_via_pair(1000.0, 0.0, amp, 0.0), level - 3.0103, 0.02);
}

/*
 * A diffuse room, the other way the two mics differ: the same level at both
 * but waveforms with nothing in common, as reverberant sound becomes above a
 * few kHz at this spacing. Two independent white noises are read one at a
 * time (each array as both mics), then as MIC1 and MIC2 together. Together
 * is the energy mean of the two, to rounding. The sample mean read 3 dB
 * under it: uncorrelated, the cross term averages to nothing.
 */
#define NOISE_N  (4 * SOUNDLEVEL_FS)

static int16_t noise1[NOISE_N], noise2[NOISE_N];

static void make_noise(int16_t *x, uint32_t seed)
{
    for (int i = 0; i < NOISE_N; i++) {
        seed = seed * 1664525u + 1013904223u;
        x[i] = (int16_t)(((int32_t)(seed >> 16) - 32768) / 8);        /* uniform, about -20 dBFS */
    }
}

static float noise_level(const int16_t *a, const int16_t *b)
{
    soundlevel_now_t now;
    soundlevel_init(&s, OFF, true);
    for (int i = 0; i < NOISE_N; i += 1000) {
        size_t k = NOISE_N - i < 1000 ? (size_t)(NOISE_N - i) : 1000u;
        soundlevel_feed(&s, a + i, b + i, 1, k, DAY0, HMS(12, 0, 0));
    }
    soundlevel_now(&s, &now);
    return now.laeq3s_dbfs;
}

static void test_two_mics_diffuse(void)
{
    make_noise(noise1, 1u);
    make_noise(noise2, 2u);

    double l1 = noise_level(noise1, noise1);
    double l2 = noise_level(noise2, noise2);
    double lp = noise_level(noise1, noise2);
    double want = 10.0 * log10((pow(10.0, l1 / 10.0) + pow(10.0, l2 / 10.0)) / 2.0);

    printf("     noise alone at MIC1 %.3f, at MIC2 %.3f, together %.3f dBFS\n", l1, l2, lp);
    expect("the two noises are the same level, so the pair has something to agree with", fabs(l1 - l2) < 0.3);
    expect_near("uncorrelated noise at both mics reads the energy mean of the two, not 3 dB under",
                lp, want, 0.005);
}

/* ---- levels ------------------------------------------------------------- */

/* Half at 60 and half at 70 is 67.4, not 65: energy, not decibels, averages. */
static void test_laeq_is_energy_mean(void)
{
    soundlevel_now_t now;
    soundlevel_report_t r;
    long pos = 0;

    soundlevel_init(&s, OFF, true);
    blocks_at(&s, 60.0, 12, DAY0, HMS(12, 0, 0));
    blocks_at(&s, 70.0, 12, DAY0, HMS(12, 0, 1));
    soundlevel_now(&s, &now);
    expect_near("LAeq,3s of 12 blocks at 60 and 12 at 70", now.laeq3s, 67.40, 0.005);
    expect_near("LAeq,1s is the last 8 blocks only", now.laeq1s, 70.0, 0.005);
    expect_near("LAF is the last block", now.laf, 70.0, 0.005);
    expect("today reports", soundlevel_today(&s, &r));
    expect_near("today's LAeq of the same", r.laeq, 67.40, 0.005);
    expect_near("today's max", r.lmax, 70.0, 0.005);
    expect_near("today's min", r.lmin, 60.0, 0.005);

    /* The same through the samples: 1 kHz sines at -40 and -30 dBFS, whole
       blocks each, so no block straddles the step. */
    soundlevel_init(&s, OFF, true);
    feed_sine(&s, 1000.0, pow(10.0, -40.0 / 20.0), 14L * SOUNDLEVEL_BLOCK, &pos, 1000, DAY0, HMS(12, 0, 0));
    feed_sine(&s, 1000.0, pow(10.0, -30.0 / 20.0), 12L * SOUNDLEVEL_BLOCK, &pos, 1000, DAY0, HMS(12, 0, 2));
    soundlevel_now(&s, &now);
    expect_near("LAeq,3s of sines at 60 and 70 dBA", now.laeq3s, 67.40, 0.02);
}

/* L90, the level exceeded 90 % of the time: the background under the events. */
static void test_l90(void)
{
    soundlevel_report_t r;

    /* 90 blocks of background at 40, 10 of events at 70, interleaved. */
    soundlevel_init(&s, OFF, true);
    for (int i = 0; i < 100; i++) blocks_at(&s, i % 10 == 3 ? 70.0 : 40.0, 1, DAY0, HMS(12, 0, 0));
    soundlevel_today(&s, &r);
    expect_near("L90 with 90 % at 40 and 10 % at 70", r.l90, 40.0, 0.01);

    /* 80 % at 60, 20 % at 40: fewer than 90 % reach 60, so L90 is the 40. */
    soundlevel_init(&s, OFF, true);
    blocks_at(&s, 60.0, 80, DAY0, HMS(12, 0, 0));
    blocks_at(&s, 40.0, 20, DAY0, HMS(12, 0, 0));
    soundlevel_today(&s, &r);
    expect_near("L90 with 80 % at 60 and 20 % at 40", r.l90, 40.0, 0.01);

    /* 95 % at 60: 60 is exceeded (or met) 95 % of the time. */
    soundlevel_init(&s, OFF, true);
    blocks_at(&s, 40.0, 5, DAY0, HMS(12, 0, 0));
    blocks_at(&s, 60.0, 95, DAY0, HMS(12, 0, 0));
    soundlevel_today(&s, &r);
    expect_near("L90 with 95 % at 60 and 5 % at 40", r.l90, 60.0, 0.01);

    /* The convention at a boundary: ten blocks on each bin from 30.0 to 79.5.
       35.0 and up is exactly 90 %, so L90 is 35.0 -- the highest level met or
       exceeded by at least 90 % of the blocks, not the 34.5 a "lowest 10 %"
       reading would give. */
    soundlevel_init(&s, OFF, true);
    for (int k = 0; k < 100; k++) blocks_at(&s, 30.0 + 0.5 * k, 10, DAY0, HMS(12, 0, 0));
    soundlevel_today(&s, &r);
    expect_near("L90 of a uniform 30..79.5 dB spread", r.l90, 35.0, 0.01);

    /* A level between bin centres lands in the nearest: 41.2 reads 41.0 and
       41.4 reads 41.5 -- rounded, not cut down to the bin below. */
    soundlevel_init(&s, OFF, true);
    blocks_at(&s, 41.2, 10, DAY0, HMS(12, 0, 0));
    soundlevel_today(&s, &r);
    expect_near("L90 resolves to the nearest 0.5 dB (41.2)", r.l90, 41.0, 0.01);
    soundlevel_init(&s, OFF, true);
    blocks_at(&s, 41.4, 10, DAY0, HMS(12, 0, 0));
    soundlevel_today(&s, &r);
    expect_near("L90 resolves to the nearest 0.5 dB (41.4)", r.l90, 41.5, 0.01);
}

/* ---- the gate ----------------------------------------------------------- */

static int same_report(const soundlevel_report_t *a, const soundlevel_report_t *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

static void test_gate_changes_nothing(void)
{
    soundlevel_now_t before, after;
    soundlevel_report_t rb, ra;
    static uint8_t sb[SOUNDLEVEL_SAVE_BYTES], sa[SOUNDLEVEL_SAVE_BYTES];
    long pos = 0;

    soundlevel_init(&s, OFF, true);
    feed_sine(&s, 1000.0, 0.01, 4L * SOUNDLEVEL_FS, &pos, 500, DAY0, HMS(12, 0, 0));
    soundlevel_now(&s, &before);
    soundlevel_today(&s, &rb);
    soundlevel_save_day(&s, sb, sizeof sb);

    /* A second of chime, loud, a minute later so it would open a new second
       and minute if it counted. */
    soundlevel_exclude(&s, true);
    feed_sine(&s, 880.0, 0.5, 1L * SOUNDLEVEL_FS, &pos, 500, DAY0, HMS(12, 1, 0));
    expect("a block while gated is refused", !soundlevel_block(&s, ms_of_dbfs(-3.0), DAY0, HMS(12, 1, 0)));
    soundlevel_now(&s, &after);
    soundlevel_today(&s, &ra);
    soundlevel_save_day(&s, sa, sizeof sa);

    expect("gated: LAF unchanged", after.laf_dbfs == before.laf_dbfs);
    expect("gated: LAeq,1s and LAeq,3s unchanged",
           after.laeq1s_dbfs == before.laeq1s_dbfs && after.laeq3s_dbfs == before.laeq3s_dbfs);
    expect("gated: today's report unchanged", same_report(&ra, &rb));
    expect("gated: today's saved totals unchanged, histogram included", memcmp(sa, sb, sizeof sa) == 0);
    expect("gated: no new minute opened", s.minute.tod_s == HMS(12, 0, 0));
}

/*
 * A block is dropped whole if the gate was on for any part of it: here on
 * halfway through block 1 and off halfway through block 2, so both go, and
 * block 3 counts. Warm-up is fed first so the counts are the gate's alone.
 */
static void test_gate_straddles_blocks(void)
{
    long pos = 0;
    const size_t half = SOUNDLEVEL_BLOCK / 2;
    soundlevel_report_t r;

    soundlevel_init(&s, OFF, true);
    feed_sine(&s, 1000.0, 0.01, 2L * SOUNDLEVEL_BLOCK, &pos, 500, DAY0, HMS(12, 0, 0));   /* warm-up */
    feed_sine(&s, 1000.0, 0.01, (long)half, &pos, 500, DAY0, HMS(12, 0, 0));
    soundlevel_exclude(&s, true);
    feed_sine(&s, 1000.0, 0.5, (long)half, &pos, 500, DAY0, HMS(12, 0, 0));     /* block 1 ends gated */
    feed_sine(&s, 1000.0, 0.5, (long)half, &pos, 500, DAY0, HMS(12, 0, 0));
    soundlevel_exclude(&s, false);
    feed_sine(&s, 1000.0, 0.01, (long)half, &pos, 500, DAY0, HMS(12, 0, 0));    /* block 2 ends open, but was gated */
    expect("no block counted while the gate touched them", !soundlevel_today(&s, &r));
    feed_sine(&s, 1000.0, 0.01, SOUNDLEVEL_BLOCK, &pos, 500, DAY0, HMS(12, 0, 0));   /* block 3 */
    expect("the first clean block counts", soundlevel_today(&s, &r) && r.blocks == 1);
    expect_near("and it is the quiet tone, not the chime", r.lmax, 20.0 * log10(0.01) + OFF, 0.05);
}

/* ---- time --------------------------------------------------------------- */

static void test_period_edges(void)
{
    expect("06:59:59 is night",   soundlevel_period(HMS(6, 59, 59)) == SOUNDLEVEL_NIGHT);
    expect("07:00:00 is day",     soundlevel_period(HMS(7, 0, 0)) == SOUNDLEVEL_DAY);
    expect("18:59:59 is day",     soundlevel_period(HMS(18, 59, 59)) == SOUNDLEVEL_DAY);
    expect("19:00:00 is evening", soundlevel_period(HMS(19, 0, 0)) == SOUNDLEVEL_EVENING);
    expect("22:59:59 is evening", soundlevel_period(HMS(22, 59, 59)) == SOUNDLEVEL_EVENING);
    expect("23:00:00 is night",   soundlevel_period(HMS(23, 0, 0)) == SOUNDLEVEL_NIGHT);
    expect("00:00:00 is night",   soundlevel_period(0) == SOUNDLEVEL_NIGHT);
}

/*
 * A calendar day's Lnight is its 00-07 and its 23-24. Blocks either side of
 * each edge, each period at its own level so a misfiled block shows, then the
 * first block after midnight: the day rolls into `yesterday` and the new one
 * starts in the night.
 */
static void test_periods_split(void)
{
    soundlevel_report_t y, d;

    soundlevel_init(&s, OFF, true);
    blocks_at(&s, 40.0, 1, DAY0, HMS(6, 59, 59));
    blocks_at(&s, 60.0, 1, DAY0, HMS(7, 0, 0));
    blocks_at(&s, 60.0, 1, DAY0, HMS(18, 59, 59));
    blocks_at(&s, 50.0, 1, DAY0, HMS(19, 0, 0));
    blocks_at(&s, 50.0, 1, DAY0, HMS(22, 59, 59));
    blocks_at(&s, 40.0, 1, DAY0, HMS(23, 0, 0));
    blocks_at(&s, 40.0, 1, DAY0, HMS(23, 59, 59));

    expect("no yesterday before the first midnight", !soundlevel_yesterday(&s, &y));
    soundlevel_today(&s, &d);
    expect_near("Lday 07-19", d.period[SOUNDLEVEL_DAY], 60.0, 0.005);
    expect_near("Levening 19-23", d.period[SOUNDLEVEL_EVENING], 50.0, 0.005);
    expect_near("Lnight 23-07", d.period[SOUNDLEVEL_NIGHT], 40.0, 0.005);

    blocks_at(&s, 45.0, 1, DAY0 + 1, HMS(0, 0, 0));
    expect("midnight: yesterday is there", soundlevel_yesterday(&s, &y));
    expect("... dated the day before", y.day == DAY0 && y.blocks == 7);
    expect_near("... with its Lday", y.period[SOUNDLEVEL_DAY], 60.0, 0.005);
    expect_near("... its Levening", y.period[SOUNDLEVEL_EVENING], 50.0, 0.005);
    expect_near("... and its Lnight, 00:00 not in it", y.period[SOUNDLEVEL_NIGHT], 40.0, 0.005);
    soundlevel_today(&s, &d);
    expect("today starts at midnight with one block", d.day == DAY0 + 1 && d.blocks == 1);
    expect_near("... in the night", d.period[SOUNDLEVEL_NIGHT], 45.0, 0.005);
    expect("... and no day or evening yet",
           isnan(d.period[SOUNDLEVEL_DAY]) && isnan(d.period[SOUNDLEVEL_EVENING]));
}

static void test_second_and_minute_roll(void)
{
    soundlevel_report_t r;

    soundlevel_init(&s, OFF, true);
    expect("no second before one has closed", !soundlevel_last_second(&s, &r));
    for (int i = 0; i < 8; i++) blocks_at(&s, i == 5 ? 70.0 : 50.0, 1, DAY0, HMS(10, 0, 0));
    expect("the second is still open", !soundlevel_last_second(&s, &r));
    blocks_at(&s, 50.0, 1, DAY0, HMS(10, 0, 1));
    expect("the next second's first block closes it", soundlevel_last_second(&s, &r));
    expect("... it is 10:00:00 with 8 blocks", r.tod_s == HMS(10, 0, 0) && r.blocks == 8 && r.day == DAY0);
    /* 7 blocks at 50 and one at 70: 10 log10((7e5 + 1e7) / 8) = 61.26 */
    expect_near("... its LAeq,1s", r.laeq, 10.0 * log10((7.0 * 1e5 + 1e7) / 8.0), 0.005);
    expect_near("... its LAFmax", r.lmax, 70.0, 0.005);
    expect("... and no L90 for a second", isnan(r.l90));

    /* The rest of the minute, then 10:01:00. */
    for (uint32_t sec = 2; sec < 60; sec++) blocks_at(&s, 55.0, 8, DAY0, HMS(10, 0, sec));
    expect("no minute before one has closed", !soundlevel_last_minute(&s, &r));
    blocks_at(&s, 55.0, 1, DAY0, HMS(10, 1, 0));
    expect("10:01:00 closes 10:00", soundlevel_last_minute(&s, &r));
    expect("... dated 10:00 with its 473 blocks", r.tod_s == HMS(10, 0, 0) && r.blocks == 8 + 1 + 58 * 8);
    expect_near("... its min", r.lmin, 50.0, 0.005);
    expect_near("... its max", r.lmax, 70.0, 0.005);
    expect_near("... its L90 (464 of 473 blocks at 55)", r.l90, 55.0, 0.01);
}

static void test_no_clock(void)
{
    soundlevel_now_t now;
    soundlevel_report_t r;

    soundlevel_init(&s, OFF, true);
    blocks_at(&s, 50.0, 30, SOUNDLEVEL_NO_DAY, 0);
    soundlevel_now(&s, &now);
    expect("with no clock the ring still has a level", now.have);
    expect_near("... the right one", now.laeq3s, 50.0, 0.005);
    expect("... but nothing is placed in a day", !soundlevel_today(&s, &r));
    expect("... or a second", !soundlevel_last_second(&s, &r));
}

/* ---- red ---------------------------------------------------------------- */

/*
 * A quiet ring at 40 dBA, then 80 dBA. LAeq,3s reaches 75 when k of the 24
 * blocks are loud and k 1e8 + (24 - k) 1e4 >= 24 10^7.5, i.e. at k = 8. So
 * of twenty loud blocks the last thirteen are red.
 */
static void test_red_seconds(void)
{
    soundlevel_now_t now;
    soundlevel_report_t r;

    soundlevel_init(&s, OFF, true);
    blocks_at(&s, 40.0, 24, DAY0, HMS(12, 0, 0));
    blocks_at(&s, 80.0, 7, DAY0, HMS(12, 0, 1));
    soundlevel_now(&s, &now);
    expect_near("7 loud blocks: not red yet", now.red_run_s, 0.0, 1e-6);
    blocks_at(&s, 80.0, 13, DAY0, HMS(12, 0, 2));
    soundlevel_now(&s, &now);
    expect_near("20 loud blocks: red for the last 13", now.red_run_s, 13.0 / 8.0, 1e-6);
    soundlevel_today(&s, &r);
    expect_near("today's red seconds", r.red_s, 13.0 / 8.0, 1e-6);

    /* A gated block neither breaks nor extends the run. */
    soundlevel_exclude(&s, true);
    blocks_at(&s, 40.0, 50, DAY0, HMS(12, 0, 3));
    soundlevel_exclude(&s, false);
    blocks_at(&s, 80.0, 3, DAY0, HMS(12, 0, 4));
    soundlevel_now(&s, &now);
    expect_near("the run carries straight over a gated stretch", now.red_run_s, 16.0 / 8.0, 1e-6);

    blocks_at(&s, 30.0, 24, DAY0, HMS(12, 0, 5));
    soundlevel_now(&s, &now);
    expect_near("quiet again: the run is broken", now.red_run_s, 0.0, 1e-6);
}

/* ---- today.bin ---------------------------------------------------------- */

static void fill_a_day(soundlevel_t *m, int32_t day)
{
    blocks_at(m, 38.0, 40, day, HMS(3, 0, 0));
    blocks_at(m, 62.0, 30, day, HMS(9, 30, 0));
    blocks_at(m, 80.0, 30, day, HMS(9, 31, 0));
    blocks_at(m, 51.5, 20, day, HMS(20, 0, 0));
}

static void test_save_restore(void)
{
    static uint8_t buf[SOUNDLEVEL_SAVE_BYTES], bad[SOUNDLEVEL_SAVE_BYTES];
    soundlevel_report_t ra, rb;

    soundlevel_init(&s, OFF, true);
    fill_a_day(&s, DAY0);
    expect("too small a buffer saves nothing", soundlevel_save_day(&s, buf, sizeof buf - 1) == 0);
    expect("saves SOUNDLEVEL_SAVE_BYTES", soundlevel_save_day(&s, buf, sizeof buf) == SOUNDLEVEL_SAVE_BYTES);

    /* The reboot: a fresh instance, nothing counted yet, and the totals back. */
    soundlevel_init(&t, OFF, true);
    expect("restores into a fresh day", soundlevel_restore_day(&t, buf, sizeof buf));
    soundlevel_today(&s, &ra);
    soundlevel_today(&t, &rb);
    expect("... today reads the same after the reboot", same_report(&ra, &rb));

    /* Both carry on through the evening: still the same. */
    blocks_at(&s, 70.0, 10, DAY0, HMS(21, 0, 0));
    blocks_at(&t, 70.0, 10, DAY0, HMS(21, 0, 0));
    soundlevel_today(&s, &ra);
    soundlevel_today(&t, &rb);
    expect("... and after more blocks on top", same_report(&ra, &rb));
    expect_near("... L90 survived the trip (histogram saved)", rb.l90, 38.0, 0.01);

    /* A torn or foreign file is refused, and changes nothing. */
    memcpy(bad, buf, sizeof bad);
    bad[100] ^= 0x10;
    soundlevel_init(&t, OFF, true);
    expect("a flipped bit is refused", !soundlevel_restore_day(&t, bad, sizeof bad));
    expect("a short file is refused", !soundlevel_restore_day(&t, buf, sizeof buf - 1));
    expect("... and nothing was restored", !soundlevel_today(&t, &rb));

    /* Restored after counting has started, on the same date: added. */
    soundlevel_init(&t, OFF, true);
    blocks_at(&t, 62.0, 5, DAY0, HMS(21, 30, 0));
    expect("restores into a day already counting the same date", soundlevel_restore_day(&t, buf, sizeof buf));
    soundlevel_today(&t, &rb);
    expect("... the blocks add up", rb.blocks == 120 + 5);

    /* Restored after counting has started on another date: refused. */
    soundlevel_init(&t, OFF, true);
    blocks_at(&t, 62.0, 5, DAY0 + 1, HMS(0, 5, 0));
    expect("refuses a save from another date once counting", !soundlevel_restore_day(&t, buf, sizeof buf));

    /* A reboot across midnight: yesterday's save, then today's first block. */
    soundlevel_init(&t, OFF, true);
    soundlevel_restore_day(&t, buf, sizeof buf);
    blocks_at(&t, 45.0, 1, DAY0 + 1, HMS(0, 0, 10));
    soundlevel_today(&s, &ra);        /* s still holds DAY0, plus the 21:00 blocks */
    expect("a restored yesterday rolls into yesterday", soundlevel_yesterday(&t, &rb) && rb.day == DAY0);
    expect("... with its blocks", rb.blocks == 120);
}

/* ---- calibration -------------------------------------------------------- */

static void test_calibration(void)
{
    soundlevel_now_t now;
    soundlevel_report_t r0, r1;
    long pos = 0;
    float off;

    /*
     * The estimate from its parts (see soundlevel.c): the ES7210's full scale
     * is 2 AVDD / 3.3 Vrms = 2.0 Vrms at AVDD 3.3 V, a sine, so +6.02 dBV is
     * 0 dBFS; with 24 dB of PGA and a -38 dBV/Pa mic, 94 dB SPL is
     * -38 + 24 - 6.02 = -20.02 dBFS, and the offset is 94 + 20.02.
     */
    expect_near("the estimated offset follows from the datasheet figures",
                SOUNDLEVEL_CAL_EST_DB, 94.0 - (-38.0 + 24.0 - 20.0 * log10(2.0 * 3.3 / 3.3)), 0.05);

    soundlevel_init(&s, SOUNDLEVEL_CAL_EST_DB, false);
    expect_near("nothing measured: calibrate changes nothing", soundlevel_calibrate(&s, 60.0f), SOUNDLEVEL_CAL_EST_DB, 0);
    expect("... and it is still an estimate", !s.calibrated);

    feed_sine(&s, 1000.0, pow(10.0, -40.0 / 20.0), 4L * SOUNDLEVEL_FS, &pos, 800, DAY0, HMS(12, 0, 0));
    soundlevel_now(&s, &now);
    expect_near("before: -40 dBFS reads as the estimate says", now.laeq3s, -40.0 + SOUNDLEVEL_CAL_EST_DB, 0.02);
    soundlevel_today(&s, &r0);

    expect_near("an impossible room (5 dBA, a typo for 50) is refused",
                soundlevel_calibrate(&s, 5.0f), SOUNDLEVEL_CAL_EST_DB, 0);
    expect_near("... as is NAN", soundlevel_calibrate(&s, NAN), SOUNDLEVEL_CAL_EST_DB, 0);

    off = soundlevel_calibrate(&s, 65.0f);
    expect_near("`!cal 65` with the room at -40 dBFS: offset 105", off, 105.0, 0.02);
    soundlevel_now(&s, &now);
    expect("... now calibrated", now.calibrated && s.calibrated);
    expect_near("... and LAeq,3s reads 65", now.laeq3s, 65.0, 1e-3);
    soundlevel_today(&s, &r1);
    expect_near("... and the whole day moves with it, not just from now",
                r1.laeq - r0.laeq, off - SOUNDLEVEL_CAL_EST_DB, 1e-3);

    /* A room that is not steady: 16 blocks at -50 dBFS, then 8 at -40. The
       fast level is -40, but the meter beside it averaged, as LAeq,3s does:
       10 log10((16e-5 + 8e-4) / 24) = -43.98 dBFS. `!cal 60` must be held
       against that, not against the last eighth of a second. */
    soundlevel_init(&s, SOUNDLEVEL_CAL_EST_DB, false);
    blocks_at(&s, -50.0 + OFF, 16, DAY0, HMS(12, 0, 0));
    blocks_at(&s, -40.0 + OFF, 8, DAY0, HMS(12, 0, 2));
    off = soundlevel_calibrate(&s, 60.0f);
    expect_near("calibrates against LAeq,3s, not LAF",
                off, 60.0 - 10.0 * log10((16.0 * 1e-5 + 8.0 * 1e-4) / 24.0), 0.005);
}

int main(void)
{
    test_design_against_iec();
    test_design_in_time();
    test_dbfs_of_sine();
    test_100hz_reads_low();
    test_feed_any_n();
    test_two_mics_tone();
    test_two_mics_diffuse();
    test_laeq_is_energy_mean();
    test_l90();
    test_gate_changes_nothing();
    test_gate_straddles_blocks();
    test_period_edges();
    test_periods_split();
    test_second_and_minute_roll();
    test_no_clock();
    test_red_seconds();
    test_save_restore();
    test_calibration();

    if (failures) { printf("\n%d FAILED\n", failures); return 1; }
    printf("\nall passed\n");
    return 0;
}
