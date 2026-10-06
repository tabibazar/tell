#ifndef LCD_H
#define LCD_H

/*
 * panel1's 4" 480x480 ST7701 panel on the S3's 16-bit RGB bus. Drawing goes
 * into a canvas in PSRAM; lcd_show() copies it to the panel's own frame in
 * one go, so a half-drawn page is never seen.
 */
#include "canvas.h"
#include "esp_err.h"

esp_err_t lcd_init(void);
canvas_t *lcd_canvas(void);
void lcd_show(void);
void lcd_backlight(int percent);   /* 0..100 */

#endif /* LCD_H */
