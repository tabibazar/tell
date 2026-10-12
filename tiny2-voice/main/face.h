#ifndef FACE_H
#define FACE_H

/*
 * tiny2's face on a 1.28" round GC9A01 (240 x 240, SPI): two eyes that blink
 * and glance about, and a mouth that opens with the loudness of the speech as
 * it plays (level_fn), smiling when quiet. Under it, while it speaks, the
 * voice's waveform scrolls by; when quiet, the time sits there instead.
 *   SCL 39, SDA 40, CS 41, DC 42, RST 38; the backlight is tied to VCC.
 */
#include "esp_err.h"

esp_err_t face_init(float (*level_fn)(void));   /* level_fn: 0..1 while speaking, <0 when silent */

#endif /* FACE_H */
