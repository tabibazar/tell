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
 * calling whenever a fresh temperature and humidity are to hand.
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
