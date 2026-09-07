#ifndef CANVAS_H
#define CANVAS_H

#include <stdint.h>
#include <stddef.h>

/*
 * A framebuffer and the text rendering that goes on it. Knows nothing about
 * panels, SPI, or RGB timings, so it is shared by every board and can be
 * tested on the host.
 */
typedef struct {
    uint16_t *fb;
    int w, h;
    int scale;         /* integer font magnification */
    int cols, rows;    /* character cells available at that scale */
    int cell_w, cell_h; /* pixel size of one cell, so callers need no font.h */
} canvas_t;

#define CANVAS_FG 0xFFFF
#define CANVAS_BG 0x0000

/* `fb` must hold w*h uint16_t. `scale` magnifies the font by pixel
   replication; 1 is the raw font cell. */
void canvas_init(canvas_t *c, uint16_t *fb, int w, int h, int scale);

void canvas_clear(canvas_t *c);

/* Wrapped body text. NULL or empty clears. */
void canvas_text(canvas_t *c, const char *utf8);

/* A single short line, centred, at the largest whole scale that fits. */
void canvas_big(canvas_t *c, const char *text);

/* The pixel size canvas_big would use for this text. */
void canvas_big_size(canvas_t *c, const char *text, int *w, int *h);

/* As canvas_big, but at a given top-left corner. Used by the screensaver to
   move the clock around so no pixel stays lit. */
void canvas_big_at(canvas_t *c, const char *text, int ox, int oy);

/* Filled rectangle in framebuffer pixels. Clipped to the panel. */
void canvas_fill_rect(canvas_t *c, int x, int y, int w, int h, uint16_t colour);

/* Draws the moon at `phase` (0 new, 0.5 full, 1 new again) as a disc of
   radius r centred at (cx, cy): the lit part in `lit`, the rest in `dark`. */
void canvas_moon(canvas_t *c, int cx, int cy, int r, float phase,
                 uint16_t lit, uint16_t dark);

/* One line of text at a character cell, in the given colour. Not wrapped;
   clipped at the right edge. */
void canvas_puts(canvas_t *c, int col, int row, const char *s, uint16_t colour);

#endif /* CANVAS_H */
