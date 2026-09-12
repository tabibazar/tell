#ifndef DS3231_H
#define DS3231_H

#include <stdbool.h>
#include <stdint.h>

/*
 * A DS3231 real-time clock on the shared I2C bus, so the board knows the time
 * from the moment it powers up instead of waiting for a Mac to sync it.
 *
 * The board only ever deals in seconds since local midnight, and so does
 * this: the Mac's sync (which is NTP-disciplined) is written to the chip as
 * local time of day with a fixed dummy date, and read back the same way. The
 * date fields are never used. Local time means the chip is an hour off for a
 * while after a daylight-saving change, until the next sync from a Mac; that
 * normally happens within minutes.
 */

/* The chip's seven time registers, 0x00..0x06, from a time of day. Pure. */
void ds3231_pack(uint32_t secs_since_midnight, uint8_t regs[7]);

/* A time of day from the seven registers. Handles both 12- and 24-hour
   modes. False if the fields are not valid BCD time. Pure. */
bool ds3231_unpack(const uint8_t regs[7], uint32_t *secs_since_midnight);

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "i2cbus.h"

/* Attaches to the chip. ESP_ERR_NOT_FOUND when nothing answers at 0x68;
   the caller carries on without a clock. i2cbus_init must have run. */
esp_err_t ds3231_init(void);

/* The time of day from the chip. False if there is no chip, the read
   failed, or the chip reports its oscillator stopped (a dead or missing
   battery), in which case the time is not to be trusted. */
bool ds3231_read(uint32_t *secs_since_midnight);

/* Sets the chip and marks its time as trustworthy again. */
bool ds3231_write(uint32_t secs_since_midnight);
#endif

/*
 * What the chip knows about itself, for the RTC page.
 *
 * The temperature is the interesting one: the DS3231 is temperature
 * compensated, and it measures its own crystal every 64 seconds to do it.
 * That reading is the reason it holds +-2ppm where a bare crystal drifts with
 * the room, so it is worth showing rather than hiding.
 */

/* Which bus the chip was found on, so a page can say where it is. Only
   meaningful once ds3231_init has succeeded. */
i2cbus_id_t ds3231_bus(void);

/* Crystal temperature in Celsius, to a quarter of a degree. */
bool ds3231_temperature(float *celsius);

/* The aging register: a factory-or-user trim, about 0.1 ppm per step. */
bool ds3231_aging(int8_t *offset);

/* True if the oscillator has stopped since the time was last set, which
   means the chip's time cannot be trusted -- a flat backup cell, usually. */
bool ds3231_stopped(bool *stopped);

#endif /* DS3231_H */
