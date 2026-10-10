#ifndef TOUCH_H
#define TOUCH_H

/* panel1's GT911, read for where a finger is now: enough for swipes. */
#include <stdbool.h>
#include "esp_err.h"

esp_err_t touch_init(void);       /* after lcd_init: it brings up the I2C bus */

/* True while a finger is down, with its position in panel pixels. */
bool touch_read(int *x, int *y);

#endif /* TOUCH_H */
