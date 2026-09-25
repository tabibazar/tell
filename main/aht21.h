#ifndef AHT21_H
#define AHT21_H

#include <stdbool.h>
#include <stdint.h>

/*
 * An Aosong AHT21: temperature and humidity on the I2C bus, at 0x38.
 *
 * It arrives on the same module as the ENS160, and it is there for a reason
 * beyond redundancy with whatever else the board has: the ENS160 wants to be
 * told the temperature and humidity *at the sensing element* to compensate
 * its readings, and the AHT21 is the part sitting a few millimetres from it.
 * A BME280 across the board measures a different microclimate.
 *
 * The chip reports twenty bits each of humidity and temperature packed into
 * five bytes -- they share a byte in the middle, a nibble each -- with a CRC
 * over the lot. The unpacking and the CRC are pure and live here, tested on
 * the host, because a nibble taken from the wrong half gives a reading that
 * is wrong but entirely plausible.
 */

#define AHT21_ADDR 0x38

/* Status byte bits, as the chip reports them. */
#define AHT21_STATUS_BUSY  0x80
#define AHT21_STATUS_CAL   0x08

/*
 * Unpacks a measurement. `d` is the six bytes from status through the last
 * data byte -- the CRC is checked separately, so this stays arithmetic.
 *
 *   d[0]      status
 *   d[1..3]   humidity, twenty bits: all of d[1], all of d[2], the high
 *             nibble of d[3]
 *   d[3..5]   temperature, twenty bits: the low nibble of d[3], then d[4]
 *             and d[5]
 */
void aht21_convert(const uint8_t d[6], float *celsius, float *humidity);

/* The chip's CRC8: polynomial 0x31, seeded 0xFF. */
uint8_t aht21_crc(const uint8_t *data, int len);

/*
 * A count of frames that failed their CRC, reported as one line every ten
 * minutes rather than a warning per read.
 *
 * With every module on envo's bus every single frame failed, which at a read
 * every thirty seconds buried the log in identical warnings. What is worth
 * knowing is the rate -- all of them, or one in fifty -- and what a bad frame
 * looks like, so the line carries the count and the newest bad frame's bytes.
 * The failed frames themselves are still never used, for either value: on the
 * bench they decoded to 5-20 % in a 70 % room and to 0 C.
 */
#define AHT21_REPORT_US (10LL * 60 * 1000000)

typedef struct {
    bool    started;
    int64_t since_us;       /* when the window being counted began */
    int     reads, bad;     /* frames checked in it, and those that failed */
    uint8_t last_bad[7];    /* the newest failed frame, its CRC byte last */
} aht21_tally_t;

/* Counts one frame of seven bytes, status through CRC. */
void aht21_tally_note(aht21_tally_t *t, int64_t now_us, const uint8_t frame[7],
                      bool crc_ok);

/*
 * Once a window has run its ten minutes: writes the summary into `out` and
 * returns true if anything failed in it, and starts the next window either
 * way. A clean window says nothing -- silence is the good news.
 */
bool aht21_tally_report(aht21_tally_t *t, int64_t now_us, char *out, int size);

/*
 * Which frames that passed their CRC deserve belief.
 *
 * A CRC of eight bits cannot tell every damaged frame from a good one: about
 * one damaged frame in 256 matches its CRC by chance (1 in 259 when bits were
 * dropped at random from a real frame, the damage envo's bus does). On a bus
 * that damages every frame, then, the frames that pass are all garbage -- a
 * read every thirty seconds would log ten or so a day, each one a 0 C or a
 * 5 % RH straight into the log and the gas sensor's compensation. So a frame
 * is used only when all of these hold:
 *
 *  - Its CRC is good, its calibrated bit is set and its temperature is inside
 *    the part's rated -40..85 C. A calibrated AHT21 always reports the bit,
 *    so a good CRC without it is damage the CRC missed; so is a reading the
 *    part cannot make. Either counts as a failed frame. (A busy bit is not
 *    damage, only a measurement not finished: dropped, but not counted.)
 *
 *  - No more than half of the last AHT21_HISTORY frames failed. On a bus
 *    that fails most frames, the ones that pass are mostly luck, so none is
 *    used until the rate recovers. This is what keeps envo's all-failing bus
 *    out of the log.
 *
 *  - It agrees -- within AHT21_AGREE_C and AHT21_AGREE_RH -- with the frame
 *    immediately before it, which must have been sound too. Two chance passes
 *    in a row are one in 67,000, and even then must agree. A frame with no
 *    such neighbour is not refused: the verdict asks for a second measurement
 *    straight away, and uses the pair if they agree. A healthy sensor so
 *    loses nothing, and a real jump -- a breath on it -- costs eighty
 *    milliseconds rather than the reading.
 *
 * No room band: this board may log a cold shed or a steamy bathroom, and
 * the bench's 24.95 C / 20 % chance pass would sail through one anyway.
 */
#define AHT21_HISTORY   16          /* frames the failure rate is taken over */
#define AHT21_AGREE_C   1.0f
#define AHT21_AGREE_RH  5.0f
#define AHT21_MIN_C     (-40.0f)    /* the part's rated range */
#define AHT21_MAX_C     85.0f

typedef enum {
    AHT21_DROP = 0,     /* not a reading */
    AHT21_USE,          /* a reading: *celsius and *humidity are set */
    AHT21_CONFIRM,      /* sound but alone: measure again now and ask again */
} aht21_verdict_t;

typedef struct {
    uint16_t failed;        /* one bit per recent frame, newest lowest: set if it failed */
    int      frames;        /* how many of those bits are real, up to AHT21_HISTORY */
    bool     prev_ok;       /* the frame just before was sound, and is below */
    float    prev_c, prev_rh;
} aht21_vet_t;

/* Judges one frame of seven bytes, status through CRC, and remembers it for
   the next. Zeroed state is a fresh start. `celsius` and `humidity` are
   written only on AHT21_USE. */
aht21_verdict_t aht21_vet(aht21_vet_t *v, const uint8_t frame[7],
                          float *celsius, float *humidity);

/* False while more than half the recent frames failed. */
bool aht21_vet_trusted(const aht21_vet_t *v);

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "i2cbus.h"

/* Looks for the chip on both buses. Not finding one is not a failure. */
esp_err_t aht21_init(void);
bool aht21_present(void);
i2cbus_id_t aht21_bus(void);

/* One reading that has passed aht21_vet. Blocks for about eighty
   milliseconds, which is what the chip takes -- a hundred and sixty when the
   first frame needs a second to vouch for it. Either argument may be NULL. */
bool aht21_read(float *celsius, float *humidity);
#endif

#endif /* AHT21_H */
