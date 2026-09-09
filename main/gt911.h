#ifndef GT911_H
#define GT911_H

#include "esp_err.h"
#include <stdbool.h>

/* Probes both known addresses. Returns ESP_ERR_NOT_FOUND when the controller
   does not answer; the caller carries on without touch. */
esp_err_t gt911_init(void);

/* True once per press, reported on release. Poll this. */
bool gt911_tapped(void);

/* Where the last tap landed, in controller coordinates. Only meaningful
   after gt911_tapped() has returned true. */
void gt911_point(int *x, int *y);

/* Diagnostic one-liner: probe result, last status byte, counters, for showing
   on the screen when no serial cable is attached. */
const char *gt911_debug(void);

#endif /* GT911_H */
