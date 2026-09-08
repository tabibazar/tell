#ifndef QMI8658_H
#define QMI8658_H

#include "esp_err.h"
#include <stdbool.h>

/* QMI8658 six-axis IMU on the Feather's STEMMA QT bus. Accelerometer in g,
   gyroscope in degrees per second, in the sensor's own frame. Mapping those
   axes onto the panel is the caller's job, because it depends on how the
   breakout is oriented relative to the screen. */
typedef struct {
    float ax, ay, az;
    float gx, gy, gz;
} qmi8658_sample_t;

/* Probes both addresses, checks WHO_AM_I, and configures both sensors.
   Returns ESP_ERR_NOT_FOUND when no IMU is attached, which is not fatal:
   the board simply does without. */
esp_err_t qmi8658_init(void);

bool qmi8658_present(void);

esp_err_t qmi8658_read(qmi8658_sample_t *out);

/* One line of state, for drawing on the panel during bring-up. */
const char *qmi8658_debug(void);

#endif /* QMI8658_H */
