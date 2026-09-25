#ifndef SOUNDLEVEL_H
#define SOUNDLEVEL_H

/*
 * speaker's sound level meter: samples in, A-weighted levels out.
 *
 * The caller hands over both mics' int16 samples at 16 kHz, MIC1 and MIC2
 * side by side. Each is A-weighted on its own and their ENERGIES are
 * averaged, never the samples: the capsules are 34.5 mm apart, and the mean
 * of two signals that do not arrive in phase reads low (see soundlevel.c).
 * Out come what the ring, the chime and the SD log need: the fast level
 * (LAF, one 125 ms block), the energy means over the last 1 s and 3 s, and
 * per second, per minute and per day the LAeq, the loudest and quietest
 * block, L90 and the seconds spent in the red. The day is split the way
 * noise maps split it: Lday 07-19, Levening 19-23, Lnight 23-07.
 *
 * Everything inside is dBFS, and the calibration offset is added only when a
 * level is read out (dBA = dBFS + cal_offset). So a `!cal` in the middle of
 * the day moves the whole day's figures together rather than leaving the
 * morning on one scale and the afternoon on another, and today's totals saved
 * to the card stay valid across a recalibration.
 *
 * Pure: no clock, no I2S, no malloc. The caller brings the samples and says
 * what the local time is; the module does the arithmetic and the rolling-over
 * of seconds, minutes and days. See
 * docs/superpowers/specs/2026-09-25-speaker-noise-monitor-design.md.
 *
 * Not thread-safe: the audio task feeds it and the LED and logger tasks read
 * it, so the app holds one lock around both, or copies a snapshot out.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOUNDLEVEL_FS          16000                     /* Hz; the filter is designed for this rate only */
#define SOUNDLEVEL_BLOCK       (SOUNDLEVEL_FS / 8)       /* samples in one 125 ms block */
#define SOUNDLEVEL_BLOCKS_1S   8
#define SOUNDLEVEL_RING        24                        /* 3 s of blocks, for LAeq,3s */

/* The histogram behind L90: 0.5 dB bins centred on -110.0, -109.5 .. +10.0
   dBFS. With the estimated offset that is about 4 to 124 dBA, wider than
   anything the mic can report at either end. */
#define SOUNDLEVEL_HIST_LO_DBFS  (-110.0f)
#define SOUNDLEVEL_HIST_STEP_DB  0.5f
#define SOUNDLEVEL_BINS          241

/* Red on the ring's scale: LAeq,3s at or above this. */
#define SOUNDLEVEL_RED_DBA     75.0f

/* The offset before anyone has calibrated, and how far it can be out. The
   derivation is in soundlevel.c; the log marks levels "est" until `!cal`. */
#define SOUNDLEVEL_CAL_EST_DB  114.0f
#define SOUNDLEVEL_CAL_EST_TOL_DB  6.0f

/* A `day` below zero: the RTC has no valid date yet. Levels still update, but
   nothing is added to the second, minute or day, which could not be placed. */
#define SOUNDLEVEL_NO_DAY      (-1)

/* The bytes soundlevel_save_day writes: speaker/today.bin. */
#define SOUNDLEVEL_SAVE_BYTES  1024

typedef enum {
    SOUNDLEVEL_DAY = 0,         /* 07:00-19:00 */
    SOUNDLEVEL_EVENING,         /* 19:00-23:00 */
    SOUNDLEVEL_NIGHT,           /* 23:00-07:00 */
    SOUNDLEVEL_NPERIOD
} soundlevel_period_t;

/* Which period a local time of day (seconds since midnight) falls in. */
soundlevel_period_t soundlevel_period(uint32_t tod_s);

/* One second-order section, transposed direct form II, in float: the S3's FPU
   is single precision, and this runs sixteen thousand times a second on
   each mic. */
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } soundlevel_biquad_t;

/*
 * One interval being added up -- a second, a minute or a day -- or one that
 * has closed. Energies are sums of block mean squares in full-scale units
 * (samples scaled to +-1), in double: they are touched eight times a second,
 * not sixteen thousand, and a day's sum of 691,200 blocks would lose the
 * quiet ones against the loud ones in float.
 */
typedef struct {
    int32_t  day;                               /* days since 1970-01-01, local */
    uint32_t tod_s;                             /* the interval's first second: the second itself, the minute's :00, 0 for a day */
    uint32_t blocks[SOUNDLEVEL_NPERIOD];
    double   energy[SOUNDLEVEL_NPERIOD];
    float    max_dbfs, min_dbfs;                /* the loudest and quietest block */
    uint32_t red_blocks;
    float    l90_dbfs;                          /* set when the interval closes */
} soundlevel_acc_t;

typedef struct {
    soundlevel_biquad_t aw[2][3];               /* the A-weighting, a chain for each mic */

    /* The block being gathered. */
    float    sum;                               /* of squared weighted samples, both mics */
    uint32_t n;
    bool     exclude;                           /* the caller's gate, as it stands now */
    bool     tainted;                           /* the gate was on at some point during this block */
    int      warmup;                            /* blocks still to drop after init */

    /* The last included blocks, as mean squares, oldest first from head. */
    float    ring[SOUNDLEVEL_RING];
    int      ring_n, ring_head;
    float    laf_dbfs;
    uint32_t red_run;                           /* consecutive included blocks that were red */

    float    cal_offset;
    bool     calibrated;

    soundlevel_acc_t second, last_second;
    soundlevel_acc_t minute, last_minute;
    soundlevel_acc_t day, yesterday;
    uint32_t minute_hist[SOUNDLEVEL_BINS];
    uint32_t day_hist[SOUNDLEVEL_BINS];
} soundlevel_t;

/* Designs the filter and starts empty. `cal_offset_db` is the stored offset,
   or SOUNDLEVEL_CAL_EST_DB with `calibrated` false before the first `!cal`. */
void soundlevel_init(soundlevel_t *s, float cal_offset_db, bool calibrated);

/*
 * Adds `n` frames, any number: a block that is not finished carries over to
 * the next call. Frame i is MIC1's sample mic1[i * stride] and MIC2's
 * mic2[i * stride], so the I2S buffer goes in as it came, with no copy. The
 * 4-slot TDM read the pinout doc gives, [ref, MIC1, unused, MIC2], is
 *
 *     soundlevel_feed(s, buf + 1, buf + 3, 4, frames, day, tod_s);
 *
 * and the datasheet's slot order, CH1 CH3 CH2 CH4, would be buf + 0 and
 * buf + 2. Two arrays of their own are stride 1. `stride` is at least 1.
 *
 * Blocks that finish in this call are placed at the local time given --
 * `day` from timecalc_days, `tod_s` seconds since midnight -- or nowhere in
 * time if `day` is SOUNDLEVEL_NO_DAY. Returns the number of blocks that
 * finished and counted (excluded ones do not).
 *
 * The first two blocks after init are dropped: the filter starting from rest
 * rings for a few tens of milliseconds and would read as a click.
 */
int soundlevel_feed(soundlevel_t *s, const int16_t *mic1, const int16_t *mic2, size_t stride, size_t n,
                    int32_t day, uint32_t tod_s);

/*
 * The gate, for the chime: on from just before the amp is enabled, off 150 ms
 * after it is disabled. Any block the gate was on for, even for one sample, is
 * dropped whole and changes nothing at all -- not the ring, not a histogram,
 * not a minimum -- so the chime is neither logged nor shown.
 */
void soundlevel_exclude(soundlevel_t *s, bool on);

/* A finished block's mean square (weighted samples scaled to +-1), placed at
   a local time. What soundlevel_feed calls; public so the statistics can be
   tested block by block. Returns false (and changes nothing) while the gate is
   on. */
bool soundlevel_block(soundlevel_t *s, float mean_square, int32_t day, uint32_t tod_s);

/* What the ring and the serial line show. dBA fields are NAN until a block has
   counted. */
typedef struct {
    bool  have;
    float laf, laeq1s, laeq3s;                  /* dBA */
    float laf_dbfs, laeq1s_dbfs, laeq3s_dbfs;
    float red_run_s;                            /* how long LAeq,3s has been red without a break */
    float cal_offset;
    bool  calibrated;
} soundlevel_now_t;

void soundlevel_now(const soundlevel_t *s, soundlevel_now_t *out);

/* An interval, in dBA with the offset as it is now. A level with no blocks
   behind it is NAN: a period of the day not reached yet, for instance. */
typedef struct {
    int32_t  day;
    uint32_t tod_s;
    uint32_t blocks;
    float    laeq, lmax, lmin, l90;
    float    period[SOUNDLEVEL_NPERIOD];        /* Lday, Levening, Lnight */
    float    red_s;
} soundlevel_report_t;

/* The last closed second (the detail log's line), the last closed minute,
   today so far, and the day before the last rollover. False when there is
   none. L90 of a second is left NAN: eight blocks do not make a percentile. */
bool soundlevel_last_second(const soundlevel_t *s, soundlevel_report_t *out);
bool soundlevel_last_minute(const soundlevel_t *s, soundlevel_report_t *out);
bool soundlevel_today(const soundlevel_t *s, soundlevel_report_t *out);
bool soundlevel_yesterday(const soundlevel_t *s, soundlevel_report_t *out);

/*
 * `!cal NN`: the room is NN dBA now. Sets the offset so that LAeq,3s reads
 * NN, marks it calibrated and returns the new offset. With nothing measured
 * yet, or an NN that is not a plausible room (outside 10-130 dBA), nothing
 * changes and the old offset is returned.
 *
 * NN is the room's own sound as a reference meter beside the board reads
 * it, and both mics hear it. A calibrator sealed over ONE port is not that:
 * one mic hears its 94 dB and the other only the room, so the energy mean
 * reads 3.01 dB under the calibrator, and `!cal 94` would put every reading
 * after it 3 dB high. With a calibrator on one port, say `!cal 91`.
 */
float soundlevel_calibrate(soundlevel_t *s, float measured_now_dba);

/* Today's totals as SOUNDLEVEL_SAVE_BYTES bytes, for speaker/today.bin:
   little-endian, with a magic, a version and a CRC-32, so a torn write is
   refused rather than restored. Returns the bytes written, or 0 if `cap` is
   too small. */
size_t soundlevel_save_day(const soundlevel_t *s, uint8_t *buf, size_t cap);

/*
 * Puts saved totals back after a reboot. Into an empty day they are taken as
 * they are, date and all (a saved yesterday then rolls into `yesterday` with
 * the first block of today). Into a day that has already started counting
 * the same date, they are added. False, and nothing changes, if the bytes are
 * not a valid save or belong to a different date than the one already
 * counting. Restore once per boot: twice adds twice.
 */
bool soundlevel_restore_day(soundlevel_t *s, const uint8_t *buf, size_t len);

/* The designed filter's gain at `f_hz`, in dB, from the coefficients actually
   running. For the tests and a boot-time sanity line. */
float soundlevel_aweight_db(const soundlevel_t *s, float f_hz);

#endif /* SOUNDLEVEL_H */
