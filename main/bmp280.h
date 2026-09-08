#ifndef BMP280_H
#define BMP280_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * The pressure and temperature sensor sharing the Feather's STEMMA QT bus.
 * We do not want the weather from it -- there is no calibration maths here at
 * all. We want its noise: the bottom bits of an uncompensated reading wander
 * on their own, which makes a better seed than a constant compiled into the
 * firmware, and unlike the boot timer it does not repeat when the board is
 * reset the same way twice.
 */

esp_err_t bmp280_init(void);
bool bmp280_present(void);

/* The chip id byte, for the log: 0x58 BMP280, 0x60 BME280, 0x61 BME680. */
uint8_t bmp280_id(void);

/* Raw, uncompensated 20-bit readings, exactly as the chip reports them. */
bool bmp280_raw(uint32_t *temperature, uint32_t *pressure);

/* Several readings, mixed. Zero when there is no sensor, so the caller can
   fall back rather than seeding everything with the same nothing. */
uint32_t bmp280_entropy(void);

#endif /* BMP280_H */
