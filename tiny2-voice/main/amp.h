#ifndef AMP_H
#define AMP_H

/*
 * Sound out of one pin into tiny2's analog amplifier (GPIO14): PWM at 78 kHz,
 * 10 bits, its duty set 16,000 times a second from a timer interrupt -- so
 * the pin's average voltage follows the waveform, and the amplifier and
 * speaker, unable to follow 78 kHz, hear only that average. (The S3's PDM
 * output, a 6 MHz one-bit stream, came out as static through this amplifier:
 * far too fast for it.) Between sounds the pin is held low, ramped down and
 * up so it does not pop.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define AMP_FS 16000

esp_err_t amp_init(int gpio);
void amp_begin(void);
bool amp_write(const int16_t *mono, int samples);   /* blocks while the queue is full; scaled by the volume */
void amp_end(void);                                  /* after the queue has played out */
void amp_volume(int level);                          /* 0-100 */
int amp_get_volume(void);

#endif /* AMP_H */
