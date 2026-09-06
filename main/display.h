#ifndef DISPLAY_H
#define DISPLAY_H

#include "esp_err.h"

/* Panel geometry in character cells, given the 12x24 font on a 240x135 panel. */
#define DISPLAY_COLS 20
#define DISPLAY_ROWS 5

/* Powers the panel, brings up SPI and the ST7789, and clears the screen. */
esp_err_t display_init(void);

/* Renders `utf8` wrapped to the panel, replacing whatever was there.
   A NULL or empty string clears the screen. */
void display_show_text(const char *utf8);

/* Renders a short single line as large as it will fit, centred. Used for the
   clock, which must be readable across a room. */
void display_show_big(const char *text);

#endif /* DISPLAY_H */
