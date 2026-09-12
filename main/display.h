#ifndef DISPLAY_H
#define DISPLAY_H

#include "canvas.h"
#include "esp_err.h"

/* Brings up the panel and clears it. */
esp_err_t display_init(void);

/* Renders `utf8` wrapped to the panel, replacing what was there.
   NULL or empty clears the screen. */
void display_show_text(const char *utf8);

/* Renders a short single line as large as it will fit, centred. Used for the
   clock, which must be readable across a room. */
void display_show_big(const char *text);

/*
 * Panel brightness, 0 to 100. Driven as PWM rather than a pin held high,
 * which is both what the backlight wants and most of what the board's heat
 * is: a lit panel at full is the largest steady draw on it.
 *
 * Boards whose backlight this project does not drive ignore it.
 */
void display_set_brightness(int percent);

/* The backend's canvas, so callers can draw views into it. */
canvas_t *display_canvas(void);

/* Pushes the canvas to the panel. */
void display_blit(void);

#endif /* DISPLAY_H */
