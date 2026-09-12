#ifndef BME280_H
#define BME280_H

#include <stdbool.h>
#include <stdint.h>

/*
 * A Bosch BME280: temperature, humidity and pressure on the I2C bus.
 *
 * This board's predecessor carried a BMP280 and deliberately did no maths at
 * all -- it wanted the bottom bits of an uncompensated reading as a seed and
 * nothing more. This one is here to be read, so the compensation is the
 * point rather than something skipped.
 *
 * The chip gives raw integers and a set of factory calibration constants, and
 * every real number comes from putting the two through Bosch's formulas. That
 * arithmetic is pure, so it lives here and is tested on the host: it is
 * fiddly, it is easy to get subtly wrong, and a wrong answer looks entirely
 * plausible on a panel.
 */

/* Factory calibration, read from the chip once at startup. */
typedef struct {
    uint16_t t1;
    int16_t  t2, t3;
    uint16_t p1;
    int16_t  p2, p3, p4, p5, p6, p7, p8, p9;
    uint8_t  h1, h3;
    int16_t  h2, h4, h5;
    int8_t   h6;
    /* Set by the temperature and used by the other two, exactly as Bosch's
       reference does: pressure and humidity both need to know how warm the
       chip was when it measured. Temperature must be compensated first. */
    int32_t  t_fine;
} bme280_cal_t;

/*
 * Unpacks the calibration from the chip's two blocks: 26 bytes at 0x88 and
 * 7 at 0xE1.
 *
 * The humidity constants are the awkward part. H4 and H5 are twelve-bit
 * signed values sharing a byte between them -- H4 takes the low nibble of
 * 0xE5 and H5 the high one -- which is the single easiest thing to get wrong
 * in this driver, and gets humidity subtly out rather than obviously broken.
 */
void bme280_parse_calib(const uint8_t tp[26], const uint8_t h[7],
                        bme280_cal_t *c);

/* Degrees Celsius, and the one that must be called first: it sets t_fine. */
float bme280_temperature(bme280_cal_t *c, int32_t adc_t);

/* Hectopascals. 0 if the calibration would divide by zero. */
float bme280_pressure(const bme280_cal_t *c, int32_t adc_p);

/* Relative humidity, 0 to 100, clamped because the formula can overshoot. */
float bme280_humidity(const bme280_cal_t *c, int32_t adc_h);

/* The three raw readings out of the eight data bytes at 0xF7. Pressure and
   temperature are twenty bits, humidity sixteen. */
void bme280_raw(const uint8_t data[8], int32_t *adc_p, int32_t *adc_t,
                int32_t *adc_h);

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "i2cbus.h"

/* Looks for the chip on both buses, at 0x76 and then 0x77. Not finding one
   is not a failure: the board does without, as it does without a clock. */
esp_err_t bme280_init(void);
bool bme280_present(void);

/* 0x60 for a BME280, 0x58 for a BMP280, 0x61 for a BME680. A BMP280 has no
   humidity and will report it as zero. */
uint8_t bme280_id(void);
i2cbus_id_t bme280_bus(void);

/* One measurement. Any of the three may be NULL. */
bool bme280_read(float *celsius, float *hpa, float *humidity);
#endif

#endif /* BME280_H */
