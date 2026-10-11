#ifndef PDM_H
#define PDM_H

/*
 * Sound out of one pin: the S3's I2S0 in PDM mode turns 16 kHz samples into
 * a fast one-bit stream on the amplifier's input pin (GPIO14 on tiny2); the
 * amplifier and speaker smooth it back into sound. Between sounds the
 * channel is off and the pin held low, so the amplifier stays quiet.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define PDM_FS 16000

esp_err_t pdm_init(int gpio);
void pdm_begin(void);
bool pdm_write(const int16_t *mono, int samples);   /* scaled by the volume */
void pdm_end(void);
void pdm_volume(int level);                          /* 0-100 */
int pdm_get_volume(void);

#endif /* PDM_H */
