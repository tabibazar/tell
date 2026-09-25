#ifndef PCF85063_H
#define PCF85063_H

#include <stdbool.h>
#include <stdint.h>

#include "ds3231.h"

/*
 * watch's clock: an NXP PCF85063 at 0x51 on the main I2C bus, soldered to the
 * Waveshare ESP32-S3-Touch-LCD-1.69 beside the touch controller and the IMU.
 *
 * It answers to the ds3231_* names declared in ds3231.h, so main.c neither
 * knows nor cares which chip a board carries: on watch, pcf85063.c defines
 * ds3231_init, ds3231_read and the rest, and ds3231.c compiles only its pure
 * helpers. What the DS3231 has and this chip does not -- a crystal
 * thermometer, an aging trim in the DS3231's units -- reports as unavailable,
 * which main.c already copes with because a DS3231 read can fail too.
 *
 * The two chips lay their time out differently, which is what the pure
 * helpers here are for. The PCF85063's seven registers run from 04h, in the
 * order seconds, minutes, hours, day, weekday, month, year -- day before
 * weekday, the other way round from the DS3231 -- and it has no century bit,
 * only 2000 to 2099. Its "do not trust me" flag is not in a status register
 * either but in bit 7 of the seconds, OS, set when the oscillator stops (a flat
 * cell, or a chip that has just powered up) and cleared by writing the
 * seconds. The same registers therefore say both what the time is and whether
 * to believe it, and one read gets both.
 */

/* The seven registers 04h..0Ah from a time of day and a date, in 24-hour mode
   and with OS clear -- writing them is what clears it. True if the date went
   in; false if it was NULL or not a real day, in which case the chip's own
   power-on date is packed (Saturday 1 January 2000), which a reader rejects.
   Pure. */
bool pcf85063_pack(uint32_t secs_since_midnight, const ds3231_date_t *date,
                   uint8_t regs[7]);

/* A time of day, and optionally the date, from the seven registers. OS is
   masked here, not judged: see pcf85063_untrusted. False if the time is not
   valid BCD. `date` may be NULL, and has its year set to 0 -- which
   ds3231_date_valid rejects -- if the chip holds no real one. Assumes 24-hour
   mode, since the mode bit is in Control_1 and not among these registers.
   Pure. */
bool pcf85063_unpack(const uint8_t regs[7], uint32_t *secs_since_midnight,
                     ds3231_date_t *date);

/* Why the chip's time is not to be believed, or NULL when it is, from
   Control_1 (00h) and the seven time registers. For a log line; any non-NULL
   answer means do not use the time. Pure. */
const char *pcf85063_untrusted(uint8_t control_1, const uint8_t regs[7]);

#endif /* PCF85063_H */
