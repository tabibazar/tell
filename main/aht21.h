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

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "i2cbus.h"

/* Looks for the chip on both buses. Not finding one is not a failure. */
esp_err_t aht21_init(void);
bool aht21_present(void);
i2cbus_id_t aht21_bus(void);

/* One measurement. Blocks for about eighty milliseconds, which is what the
   chip takes; either argument may be NULL. */
bool aht21_read(float *celsius, float *humidity);
#endif

#endif /* AHT21_H */
