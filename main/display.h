#ifndef DISPLAY_H
#define DISPLAY_H

#include "esp_err.h"

/* Panel geometry in character cells, given the 8x16 font on a 240x135 panel. */
#define DISPLAY_COLS 30
#define DISPLAY_ROWS 8

/* Powers the panel, brings up SPI and the ST7789, and clears the screen. */
esp_err_t display_init(void);

/* Renders `utf8` wrapped to the panel, replacing whatever was there.
   A NULL or empty string clears the screen. */
void display_show_text(const char *utf8);

#endif /* DISPLAY_H */
