#ifndef ENS160_H
#define ENS160_H

#include <stdbool.h>
#include <stdint.h>

/*
 * A ScioSense ENS160: a metal-oxide gas sensor on the I2C bus.
 *
 * What it actually measures, and what it does not. The ENS160 heats a film of
 * metal oxide and watches its resistance change in the presence of reducing
 * gases. From that it reports TVOC in parts per billion, which is a real
 * measurement, and an air quality index of one to five.
 *
 * It also reports "eCO2", and that number needs saying plainly: it is
 * EQUIVALENT carbon dioxide, inferred from the VOC signal on the assumption
 * that in an occupied room the two rise together. It is not a CO2
 * measurement. A MOX film cannot see CO2 at all -- fill the room with pure
 * carbon dioxide and this part will report clean air. Breathe on it and eCO2
 * climbs, because breath carries VOCs, not because it counted any molecules.
 * Modules are sold with "CO2" on the label, which is where the confusion
 * starts. Real CO2 wants NDIR or photoacoustic: an SCD4x or an MH-Z19.
 *
 * Two consequences for anything that logs it:
 *
 *  - Readings are not trustworthy immediately. ScioSense specify three
 *    minutes to something reasonable, five after any restart, and a full
 *    hour after the first power-up of a new part. The chip says which state
 *    it is in and this driver passes that through rather than hiding it: a
 *    log that records an hour of nonsense after every power cut is worse than
 *    one that records a gap.
 *
 *  - It wants to be told the ambient temperature and humidity, which it uses
 *    to compensate. Without them the readings drift with the weather.
 *
 * The conversions and the status decoding are pure and tested on the host.
 */

#define ENS160_ADDR_LOW   0x52      /* ADDR strapped low */
#define ENS160_ADDR_HIGH  0x53      /* ADDR strapped high */

#define ENS160_PART_ID    0x0160
#define ENS161_PART_ID    0x0161

/* Registers, from the ScioSense datasheet. */
#define ENS160_REG_PART_ID      0x00
#define ENS160_REG_OPMODE       0x10
#define ENS160_REG_CONFIG       0x11
#define ENS160_REG_COMMAND      0x12
#define ENS160_REG_TEMP_IN      0x13
#define ENS160_REG_RH_IN        0x15
#define ENS160_REG_DATA_STATUS  0x20
#define ENS160_REG_DATA_AQI     0x21
#define ENS160_REG_DATA_TVOC    0x22
#define ENS160_REG_DATA_ECO2    0x24

#define ENS160_OPMODE_DEEP_SLEEP 0x00
#define ENS160_OPMODE_IDLE       0x01
#define ENS160_OPMODE_STANDARD   0x02
#define ENS160_OPMODE_RESET      0xF0

/* DEVICE_STATUS bits. */
#define ENS160_STATUS_NEWDAT  0x02
#define ENS160_STATUS_NEWGPR  0x01

/*
 * How much the chip trusts its own output, from bits 3:2 of DEVICE_STATUS.
 * Anything but NORMAL means the number is real data that does not yet mean
 * what it will mean, and a logger should say so rather than store it as
 * though it did.
 */
typedef enum {
    ENS160_NORMAL = 0,
    ENS160_WARMUP = 1,          /* minutes after a restart */
    ENS160_INITIAL_STARTUP = 2, /* the first hour of a new part */
    ENS160_INVALID = 3,
} ens160_validity_t;

ens160_validity_t ens160_validity(uint8_t status);
bool ens160_has_new_data(uint8_t status);

/*
 * The compensation inputs, in the chip's own units: temperature as kelvin
 * times sixty-four, humidity as per cent times five hundred and twelve. Both
 * are written little-endian, four bytes from TEMP_IN.
 */
uint16_t ens160_encode_temp(float celsius);
uint16_t ens160_encode_rh(float humidity);

/* A one-to-five air quality index as a word, for a panel with room for one. */
const char *ens160_aqi_name(uint8_t aqi);

/*
 * What to tell the chip about the air, chosen from what the board measured
 * this time round. Pure, so the choice is tested on the host rather than
 * trusted.
 *
 * Only numbers that deserve belief go in. An AHT21 frame that failed its CRC,
 * or passed it only by luck, never reaches here at all -- aht21_read refuses
 * it, see aht21_vet -- because on the bench those frames decoded to 5-20 % in
 * a room that was really 70 %, and to 0 C. Compensating with that would bend
 * every gas reading for the sake of a number the sensor itself disowned.
 *
 *   temperature  the AHT21, which sits beside the gas sensor; else the
 *                barometer's thermometer; else the last of either, if under
 *                ten minutes old; else 25 C.
 *   humidity     the AHT21 now; else its last good reading, if under ten
 *                minutes old; else a BME280's own hygrometer (never a
 *                BMP280's, which has none and reports zero); else 50 %.
 *
 * Something is always chosen, and always written. The chip keeps TEMP_IN and
 * RH_IN until they are written again, so writing nothing -- as this once did
 * with no thermometer -- left in force whatever came last, however old, and
 * the log could not say what that was. Now the chip is never compensating
 * with a number more than ten minutes old, and the log names what it is.
 *
 * Twenty-five degrees and fifty per cent are not guesses made here. The chip
 * reports what it is compensating with in DATA_T (0x30) and DATA_RH (0x32),
 * and until the host writes TEMP_IN and RH_IN those read their defaults of
 * 0x4A8A and 0x6400 -- 25 C and 50 %RH (ENS160 datasheet v1.3, SC-001224-DS-9,
 * sections 16.2.12 and 16.2.13). So writing them tells the chip what it would
 * assume anyway. The TEMP_IN and RH_IN registers themselves reset to 0x0000,
 * which would read as 0 K and 0 %, but they are inputs and not what the chip
 * uses until written.
 */
#define ENS160_DEFAULT_C    25.0f
#define ENS160_DEFAULT_RH   50.0f
#define ENS160_HOLD_US      (10LL * 60 * 1000000)   /* how long a real reading stays good */

typedef enum {
    ENS160_FROM_NONE = 0,       /* nothing chosen yet */
    ENS160_FROM_AHT21,          /* this time round, vetted */
    ENS160_FROM_AHT21_HELD,     /* the last vetted AHT21 humidity */
    ENS160_FROM_BMX280,         /* the barometer: a BMP280's or BME280's */
    ENS160_FROM_HELD,           /* the last real temperature, from either */
    ENS160_FROM_DEFAULT,        /* the chip's own 25 C or 50 % */
} ens160_source_t;

/* Everything the board measured, and how much of it can be believed. */
typedef struct {
    bool    aht_ok;             /* an AHT21 frame that passed aht21_vet, just now */
    float   aht_c, aht_rh;
    bool    held_ok;            /* the newest vetted AHT21 humidity */
    float   held_rh;
    int64_t held_age_us;        /* how long ago that was */
    bool    held_t_ok;          /* the newest real temperature, AHT21's or barometer's */
    float   held_c;
    int64_t held_t_age_us;
    bool    bmx_ok;             /* the barometer answered */
    float   bmx_c;
    bool    bmx_rh_ok;          /* and is a BME280, with a real hygrometer */
    float   bmx_rh;
} ens160_air_t;

typedef struct {
    float           celsius, humidity;  /* humidity is never zero */
    ens160_source_t t_from, rh_from;
    int64_t         t_age_us;           /* nonzero only for a held temperature */
    int64_t         rh_age_us;          /* nonzero only for a held humidity */
} ens160_comp_t;

ens160_comp_t ens160_choose_comp(const ens160_air_t *air);

/* "AHT21", "AHT21 held", "BMx280", "held", "default" or "--", for a log line. */
const char *ens160_source_name(ens160_source_t s);

/*
 * The newest gas reading, kept so that two readers a moment apart get the
 * same one.
 *
 * The chip computes a reading a second in standard mode, raises NEWDAT, and
 * clears it at the first read of the DATA registers (DEVICE_STATUS bit 1,
 * datasheet v1.3). envo reads the gas twice in one pass whenever a five-minute flash
 * sample falls in a thirty-second SD slot, which in field sleep is every
 * time; the second reader, about a hundred milliseconds behind, found NEWDAT
 * clear and got nothing. The reading it wanted is the one just taken -- the
 * newest the chip has -- so it gets that, for as long as the chip could not
 * have made a newer one. Two seconds is two of its cycles: past that a clear
 * NEWDAT means the chip has stopped, and an old number is not passed off as
 * a current one.
 */
#define ENS160_REUSE_US (2LL * 1000000)

typedef struct {
    bool              ok;
    int64_t           at_us;        /* when it was read from the chip */
    uint16_t          eco2_ppm, tvoc_ppb;
    uint8_t           aqi;
    ens160_validity_t validity;
} ens160_gas_t;

/* Keeps a reading just taken from the chip. */
void ens160_gas_keep(ens160_gas_t *last, int64_t now_us, uint16_t eco2_ppm,
                     uint16_t tvoc_ppb, uint8_t aqi, ens160_validity_t validity);

/* True if the kept reading is still the newest the chip can have made. */
bool ens160_gas_recent(const ens160_gas_t *last, int64_t now_us);

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "i2cbus.h"

/* Looks on both buses, at 0x52 then 0x53, and checks the part ID before
   believing it. Not finding one is not a failure. */
esp_err_t ens160_init(void);
bool ens160_present(void);
i2cbus_id_t ens160_bus(void);
uint16_t ens160_part_id(void);

/*
 * Tells the chip what the air around it is like, so it can compensate. Worth
 * calling every time the gas is read, with what ens160_choose_comp chose.
 * Refuses a humidity of zero or less.
 */
bool ens160_compensate(float celsius, float humidity);

/*
 * One reading. `validity` says how much to trust it and is never NULL-checked
 * away: a caller that ignores it will log an hour of rubbish after every cold
 * start. Any of the value pointers may be NULL.
 */
bool ens160_read(uint16_t *eco2_ppm, uint16_t *tvoc_ppb, uint8_t *aqi,
                 ens160_validity_t *validity);
#endif

#endif /* ENS160_H */
