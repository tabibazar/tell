#ifndef I2CBUS_H
#define I2CBUS_H

#include "driver/i2c_master.h"
#include "esp_err.h"

/*
 * The board's I2C buses, shared by every device on them.
 *
 * The touch controller used to create its bus itself, which meant a second
 * device could not be added: i2c_new_master_bus fails on a port that already
 * has one. Ownership lives here instead, so drivers only attach.
 *
 * There are two, because a board's pins are not always negotiable. wave's
 * QMI8658 is soldered to GPIO48/47 and those do not come out to her header,
 * so anything added later -- a clock chip, say -- cannot join that bus and
 * needs its own on pins that are reachable. The second bus is configured per
 * board and simply does not exist where no pins are set for it.
 */

typedef enum {
    I2CBUS_MAIN = 0,   /* whatever the board has soldered down */
    I2CBUS_AUX,        /* the header pins, when the board offers any */
    I2CBUS_COUNT,
} i2cbus_id_t;

/* Creates the bus on first call and returns ESP_OK thereafter.
   ESP_ERR_NOT_SUPPORTED if this board has no pins for that bus. */
esp_err_t i2cbus_init(i2cbus_id_t which);

/* NULL until the matching i2cbus_init has succeeded. */
i2c_master_bus_handle_t i2cbus_handle(i2cbus_id_t which);

/* Which pins a bus is on, or -1 if the board has no such bus. For anything
   that wants to report where it found a chip. */
int i2cbus_sda(i2cbus_id_t which);
int i2cbus_scl(i2cbus_id_t which);

/* Probes an address, so a driver can find which one its chip answers on. */
bool i2cbus_probe(i2cbus_id_t which, uint8_t addr);

#endif /* I2CBUS_H */
