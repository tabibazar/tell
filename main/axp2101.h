#ifndef AXP2101_H
#define AXP2101_H

#include "esp_err.h"

/*
 * The AXP2101 power-management chip on visio (Waveshare ESP32-S3-Touch-LCD-3.5B).
 *
 * Unlike lilly's GPIO15 or the Feather's GPIO21, the panel rail here is not a
 * pin the SoC toggles: it comes out of this PMIC, which sits on the board's
 * shared I2C bus (I2CBUS_MAIN, SDA 8 / SCL 7) at address 0x34. Nothing on the
 * display lights until its rails are enabled, so display_init brings this up
 * first -- the same "dead port that is really an unlit rail" trap as lilly,
 * one chip further down.
 *
 * This is not a general driver: it enables the rails Waveshare's own factory
 * firmware enables, at the voltages it sets, and does no charging, monitoring
 * or sleep. The backlight is a separate LEDC pin (GPIO6), so a wrong rail here
 * shows as a dark or dim panel, never as an over-volt.
 */

/* Brings the PMIC's rails up over I2CBUS_MAIN. Idempotent; safe to call before
   the bus exists (it creates it). ESP_OK once the rails are enabled. */
esp_err_t axp2101_init(void);

#endif /* AXP2101_H */
