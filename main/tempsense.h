#ifndef TEMPSENSE_H
#define TEMPSENSE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * The ESP32-S3's own temperature sensor. It measures the die, not the room:
 * a chip busy at 240 MHz behind a lit panel reads well above ambient, so this
 * says how hard the board is working rather than how warm the desk is.
 */

esp_err_t tempsense_init(void);

/* Degrees Celsius at the die. False if the sensor is not up. */
bool tempsense_read(float *celsius);

/*
 * Entropy from the sensor's own noise. The bottom bits of consecutive
 * readings disagree even when nothing changes, which is exactly what a seed
 * wants. The barometer used to do this job and was deleted with the board it
 * served; this needs no hardware that is not already on the die.
 *
 * Not a substitute for esp_random(), which is a real hardware RNG -- this is
 * mixed with it, not trusted alone.
 */
uint32_t tempsense_entropy(void);

#endif /* TEMPSENSE_H */
