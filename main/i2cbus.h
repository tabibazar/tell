#ifndef I2CBUS_H
#define I2CBUS_H

#include "driver/i2c_master.h"
#include "esp_err.h"

/*
 * The one I2C bus, shared by every device on it.
 *
 * The touch controller used to create the bus itself, which meant a second
 * device could not be added: i2c_new_master_bus fails on a port that already
 * has one. Ownership lives here instead, so drivers only attach.
 */

/* Creates the bus on first call and returns ESP_OK thereafter. */
esp_err_t i2cbus_init(void);

/* NULL until i2cbus_init has succeeded. */
i2c_master_bus_handle_t i2cbus_handle(void);

/* Probes an address, so a driver can find which one its chip answers on. */
bool i2cbus_probe(uint8_t addr);

#endif /* I2CBUS_H */
