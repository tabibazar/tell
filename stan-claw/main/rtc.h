#ifndef RTC_H
#define RTC_H

/* The board's PCF85063 clock chip (0x51, shared I2C bus), which keeps time
   through deep sleep far better than the ESP32's own timer. Holds UTC; set
   from SNTP whenever the network gives the time. Not fatal when absent. */
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

esp_err_t clockchip_init(void);          /* after lcd_bus_init */
bool clockchip_get(time_t *utc);         /* false when absent or never set */
void clockchip_set(time_t utc);

#endif /* RTC_H */
