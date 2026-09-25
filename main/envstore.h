#ifndef ENVSTORE_H
#define ENVSTORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Thirty days of room readings, kept in flash.
 *
 * One sample a minute of temperature, humidity and pressure, in a ring of
 * 4 KB sectors. The ring is the retention: when it wraps it erases the oldest
 * sector and writes over it, so the log is always the last thirty days and
 * never grows.
 *
 * Why a raw partition and not a file. Appending twelve bytes a minute to a
 * filesystem rewrites directory metadata on every write, and metadata blocks
 * are where flash actually wears out -- the same few sectors erased over and
 * over while the data sectors sit idle. A ring writes each byte exactly once
 * between erases, and erases each sector about once a month: something like a
 * hundred and twenty cycles in ten years, against a part rated for a hundred
 * thousand.
 *
 * Each record carries its own timestamp rather than having one inferred from
 * its position. That costs four bytes a sample and buys correctness across
 * the three things that certainly will happen: a reboot, a minute when the
 * sensor did not answer, and a clock corrected by a Mac. Any of those shifts
 * position-inferred history sideways, silently, for ever.
 *
 * Everything here is pure: flash arrives as four function pointers, so the
 * whole ring -- wrapping, recovery after a power cut, the search for where
 * writing stopped -- is exercised on the host against a fake NOR flash that
 * enforces the real rules.
 */

/* NOR flash, as this module needs it. `write` may only clear bits, which is
   what the hardware does; `erase` takes whole sectors. */
typedef struct {
    void  *ctx;
    size_t size;            /* bytes in the partition */
    size_t sector_size;     /* erase granularity */
    bool (*read)(void *ctx, size_t off, void *dst, size_t len);
    bool (*write)(void *ctx, size_t off, const void *src, size_t len);
    bool (*erase)(void *ctx, size_t off, size_t len);
} envflash_t;

/*
 * One reading. Fixed point rather than float: a float is four bytes to say
 * what two say exactly here, and "exactly" matters in a log that is compared
 * against itself a month later.
 */
/*
 * One reading, from whatever the board has. A field a board cannot measure is
 * stored as zero and marked absent in `have`, so a log can be read back
 * without knowing which sensors the board carried when it was written -- which
 * matters, because the boards differ and the log outlives them.
 */
#define ENV_HAVE_TEMP  0x01
#define ENV_HAVE_RH    0x02
#define ENV_HAVE_HPA   0x04
#define ENV_HAVE_GAS   0x08     /* tvoc, eco2 and aqi */

/*
 * Invented, not measured. Set on every reading a demo fill writes.
 *
 * A log outlives the board and the afternoon, and plausible fake weather is
 * indistinguishable from real weather a month later -- so it is marked at the
 * point of writing rather than remembered. tools/envlog.py says how many of
 * these it found, and refuses to summarise a log as though they were real.
 */
#define ENV_SYNTHETIC  0x80

/* The gas reading's worth, from the ENS160's own validity flag: 0 normal,
   1 warming up, 2 first hour from cold, 3 invalid. Stored rather than
   discarded, so an hour of settling readings can be recognised later instead
   of being mistaken for an hour of bad air. */
#define ENV_GAS_VALIDITY(flags)  (((flags) >> 4) & 0x03)
#define ENV_GAS_FLAGS(validity)  (uint8_t)(((validity) & 0x03) << 4)

typedef struct {
    uint32_t minute;        /* minutes since 1970-01-01 00:00, local */
    int16_t  temp_c100;     /* 0.01 C */
    uint16_t rh_c100;       /* 0.01 %, 0..10000 */
    uint16_t hpa_x10;       /* 0.1 hPa */
    uint16_t tvoc_ppb;      /* parts per billion */
    uint16_t eco2_ppm;      /* parts per million, and derived -- see ens160.h */
    uint8_t  aqi;           /* 1..5, the UBA index */
    uint8_t  flags;         /* ENV_HAVE_* and the gas validity */
} env_sample_t;

/*
 * The series a reading may hold, as the charts index them. Every board has an
 * entry for each; a board without the sensor simply never fills its own, and
 * the page is not offered. Fixed rather than per-board because the log is read
 * back by tools and by later firmware, and an index that means different
 * things on different boards is a trap set for one of them.
 */
#define ENV_TEMP 0
#define ENV_RH   1
#define ENV_HPA  2
#define ENV_VOC  3
#define ENV_CO2  4
#define ENV_SERIES 5

/*
 * One series out of a reading, in the log's own fixed point, and whether the
 * reading has it at all. A series whose sensor did not answer is stored as
 * zero, and zero is a real-looking value -- a reading of freezing, of bone-dry
 * air, of clean air -- so anything that charts or prints a reading asks here
 * first rather than taking the field. The case that forced this: envo with
 * its barometer pulled and its hygrometer failing every CRC still logs gas,
 * and a gas-only reading must not put a 0 C on the temperature chart.
 */
bool env_sample_value(const env_sample_t *r, int series, int16_t *out);

/*
 * envo's long-term log on the microSD: a CSV file a day, one line per 30 s
 * reading. Here rather than beside the SD code because what matters about it
 * is pure -- that every line has the columns its header names -- and that is
 * tested on the host.
 *
 * gas_valid is the ENS160's own opinion of its numbers -- 0 normal,
 * 1 warming up, 2 the first hour of a new part, 3 invalid -- so a settling
 * hour can be told from a bad-air hour later. aqi is the chip's own 1..5 UBA
 * index, which the flash log has always kept and the card had not. Each new
 * column goes last, so every column before it keeps the place it had in files
 * already on the card. Every measured column is empty when its sensor gave
 * nothing: an empty field is a gap to anything that reads the file, where
 * 0.00 is a freezing room.
 */
extern const char ENVCSV_HEADER[];      /* ends in its newline */

/* One line, newline and all, for a reading taken at `sod` seconds into the
   given day. `die_c` is the chip's own temperature, if `die_ok`. Returns the
   length written. */
int envcsv_line(char *out, size_t size, const env_sample_t *rec,
                int year, int month, int day, uint32_t sod,
                bool die_ok, float die_c);

/*
 * Which file a day's lines go in.
 *
 * The header is written only when a file is made, so a file already on the
 * card with other columns -- begun that morning by firmware from before a
 * column was added -- must not be appended to: every new line would have a
 * field its header does not name. The plain envo/YYYY-MM-DD.csv is used when
 * it is new, empty, or has today's header; else envo/YYYY-MM-DD_2.csv, _3 and
 * so on, the first that will do -- an underscore, so a later file sorts after
 * the plain one rather than before it. Deciding by the header rather than by
 * whether the file exists also stops a header being appended into the middle
 * of a file whenever the card was not yet mounted to be asked.
 *
 * `head` reads a file's first line without its line ending and returns 1,
 * 0 if there is no such file, or -1 if the card could not say. `*fresh` is
 * set when the chosen file wants the header written first. False if nothing
 * will do or the card could not say.
 */
#define ENVCSV_MAX_FILES 9
typedef int (*envcsv_head_fn)(const char *path, char *line, size_t size, void *ctx);
bool envcsv_path(char *out, size_t size, int year, int month, int day,
                 envcsv_head_fn head, void *ctx, bool *fresh);

#define ENVSTORE_RECORD   16          /* bytes on flash, serialised explicitly */
#define ENVSTORE_HDR      8           /* magic and sequence, per sector */
#define ENVSTORE_NO_MINUTE 0xFFFFFFFFu /* erased state: this slot is unwritten */

typedef struct {
    const envflash_t *f;
    int      sectors;
    int      per_sector;    /* records that fit after the header */
    int      cur;           /* sector being appended to */
    uint32_t seq;           /* its sequence number */
    int      slot;          /* next free record in it */
    bool     ready;
} envstore_t;

/*
 * Finds where the last run stopped, or formats if the partition is blank or
 * unrecognisable. Safe to call after a power cut mid-write: a half-written
 * record has an erased timestamp and is treated as the free slot it is.
 */
bool envstore_open(envstore_t *s, const envflash_t *f);

/* Appends one reading. */
bool envstore_add(envstore_t *s, const env_sample_t *sample);

/* How many readings are held, and the capacity in readings. */
int envstore_count(const envstore_t *s);
int envstore_capacity(const envstore_t *s);

/*
 * Walks every reading, oldest first. `fn` returning false stops the walk.
 * Reads a sector at a time into `buf`, which must be at least sector_size --
 * the caller owns it so this module allocates nothing.
 */
void envstore_walk(const envstore_t *s, void *buf,
                   bool (*fn)(const env_sample_t *, void *), void *ctx);

/* The newest reading, which is what a page shows as "now". */
bool envstore_latest(const envstore_t *s, env_sample_t *out);

/* Throws everything away and starts again. */
bool envstore_format(envstore_t *s);

#endif /* ENVSTORE_H */
