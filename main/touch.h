#ifndef TOUCH_H
#define TOUCH_H

#include "esp_err.h"
#include <stdbool.h>

/*
 * Whatever touch controller this board has, behind one interface.
 *
 * Two exist so far and they have nothing in common but the job: a GT911 on
 * the CrowPanel and a CST816D on envo. One of the two drivers is compiled in
 * per board -- see the excludes in CMakeLists.txt -- so main never asks which.
 *
 * Coordinates are CANVAS coordinates, not the controller's. Every panel here
 * is driven rotated, mirrored or inset relative to the glass, and a driver
 * that handed back raw controller coordinates would make every caller repeat
 * that arithmetic and get it wrong differently.
 */

/* ESP_ERR_NOT_FOUND when nothing answers; the caller carries on untouched. */
esp_err_t touch_init(void);

/* True once per press, reported on release. Poll it. */
bool touch_tapped(void);

/* Where the last tap landed, in canvas coordinates. Only meaningful just
   after touch_tapped() returned true. */
void touch_point(int *x, int *y);

/* A diagnostic one-liner, for showing on the panel when no cable is on. */
const char *touch_debug(void);

#endif /* TOUCH_H */
