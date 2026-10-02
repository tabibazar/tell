#ifndef CAM_H
#define CAM_H

/*
 * spy's OV5640, one shot at a time. Nothing on the board can cut the camera's
 * power (PWDN and RESET are strapped; only the CAM DIP kills the rail), and
 * the factory firmware, streaming without pause, made it hot. So the camera
 * is only ever up for a shot: init, let exposure settle, keep the last frame,
 * write it, then software power-down (0x3008 = 0x42) and deinit, which stops
 * XCLK too.
 */
#include <stdbool.h>
#include <stddef.h>
#include "sensor.h"

/* At boot: wake the sensor once and put it straight to sleep, since it
   powers up awake. */
void cam_sleep(void);

/* One JPEG at `size` (FRAMESIZE_VGA for the time-lapse, FRAMESIZE_QSXGA for
   a picture) written to `path`. `*bytes` gets its size. */
bool cam_shot(framesize_t size, int quality, const char *path, size_t *bytes);

/* Upside down (180 degrees: vflip + hmirror), for however spy is mounted. */
void cam_set_flip(bool flip);
bool cam_flip(void);

#endif /* CAM_H */
