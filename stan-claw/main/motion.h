#ifndef MOTION_H
#define MOTION_H

/* The board's QMI8658, accelerometer only, on the shared I2C bus: enough to
   feel a knock on the desk. Not fatal when absent: knocks just don't wake it. */
#include <stdbool.h>
#include "esp_err.h"

esp_err_t motion_init(void);                      /* after lcd_init */
bool motion_read(float *ax, float *ay, float *az); /* g; false when absent or on a bus error */

#endif /* MOTION_H */
