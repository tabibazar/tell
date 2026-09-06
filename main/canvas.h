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
    int scale;      /* integer font magnification */
    int cols, rows; /* character cells available at that scale */
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

#endif /* CANVAS_H */
