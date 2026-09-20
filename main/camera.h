#ifndef CAMERA_H
#define CAMERA_H

/*
 * envio's OV5640 camera, on the DVP header, SCCB shared with the main I2C
 * bus (I2CBUS_MAIN). envio-only: the DVP pins do not exist on any other
 * board, so every declaration and definition here is guarded by
 * CONFIG_SCREEN_HAVE_CAMERA (see Kconfig.projbuild) and this header must
 * not be included where that config is unset.
 */

#include "sdkconfig.h"

#if CONFIG_SCREEN_HAVE_CAMERA

#include "canvas.h"
#include "esp_camera.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* esp_camera_init in RGB565 QVGA (320x240) on the shared SCCB port.
   Idempotent: a second call while already started is a no-op returning
   ESP_OK. */
esp_err_t camera_start(void);

/* esp_camera_deinit. Safe to call when not started. */
void camera_stop(void);

/* Grabs one RGB565 frame and blits it centred into the canvas. Returns
   false if no frame was available (camera not started, or fb_get failed). */
bool camera_preview(canvas_t *c);

/* Switches the sensor to JPEG at capture resolution, discards a few frames
   so auto-exposure can settle, grabs one, and hands back its buffer/length
   plus the fb itself so the caller can write it out and then return it with
   esp_camera_fb_return(). Call camera_resume_preview() afterwards. */
esp_err_t camera_capture_jpeg(const uint8_t **out, size_t *len,
                              camera_fb_t **fb_to_return);

/* Switches the sensor back to RGB565 QVGA, for preview after a capture. */
void camera_resume_preview(void);

/* Switches the sensor to RGB565 at a capture resolution (larger than the
   QVGA preview), discards a few frames so auto-exposure can settle, and
   hands back the raw fb. RGB565, not JPEG, so the caller can draw onto the
   pixels (a timestamp) before encoding. Call esp_camera_fb_return(fb) and
   then camera_resume_preview() afterwards, exactly as with
   camera_capture_jpeg. */
esp_err_t camera_capture_rgb(camera_fb_t **fb_out);

/* Sets the framesize camera_capture_rgb uses for its next capture (the
   resolution picker on PAGE_CAMERA). Default FRAMESIZE_SVGA. Takes effect
   on the next capture, not retroactively. If the sensor/allocator cannot
   actually deliver the requested size, camera_capture_rgb falls back to
   FRAMESIZE_VGA and logs it rather than failing the shot outright. */
void camera_set_capture_size(framesize_t sz);

/* The framesize the resolution picker is currently set to, for the button
   label -- not necessarily the size of the last photo taken if a fallback
   happened. */
framesize_t camera_get_capture_size(void);

#endif /* CONFIG_SCREEN_HAVE_CAMERA */

#endif /* CAMERA_H */
