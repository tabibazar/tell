#ifndef DISPLAY_H
#define DISPLAY_H

#include "esp_err.h"

/* Brings up the panel and clears it. */
esp_err_t display_init(void);

/* Renders `utf8` wrapped to the panel, replacing what was there.
   NULL or empty clears the screen. */
void display_show_text(const char *utf8);

/* Renders a short single line as large as it will fit, centred. Used for the
   clock, which must be readable across a room. */
void display_show_big(const char *text);

#endif /* DISPLAY_H */
