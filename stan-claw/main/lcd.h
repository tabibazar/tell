#ifndef LCD_H
#define LCD_H

/*
 * panel1's 4" 480x480 ST7701 panel on the S3's 16-bit RGB bus. Drawing goes
 * into a canvas in PSRAM; lcd_show() copies it to the panel's own frame in
 * one go, so a half-drawn page is never seen.
 */
#include <stdbool.h>

#include "canvas.h"
#include "esp_err.h"
#include "driver/i2c_master.h"

/* The shared I2C bus alone, for a wake from deep sleep that goes straight
   back to sleep without lighting the panel. lcd_init calls it too. */
esp_err_t lcd_bus_init(void);

esp_err_t lcd_init(void);

/* Before deep sleep: the panel asleep, the amplifier off, the backlight
   held off through the sleep (lcd_init releases it). */
void lcd_off_for_sleep(void);
canvas_t *lcd_canvas(void);
void lcd_show(void);
void lcd_backlight(int percent);   /* 0..100 */
bool lcd_pwr_key(void);            /* true while the side PWR key is held */

/* The speaker amplifier, on the TCA9554's EXIO3. */
void lcd_amp(bool on);

/* The shared I2C bus (GPIO47/48), for the touch and audio drivers. */
i2c_master_bus_handle_t lcd_i2c(void);

#endif /* LCD_H */
