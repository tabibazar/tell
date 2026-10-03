#ifndef BLINKY1_AIRUI_H
#define BLINKY1_AIRUI_H

/*
 * blinky1's 0.96" OLED pages, drawn into a 128x64 one-bit frame in the
 * SSD1306's own layout (8 pages of 128 columns, a byte a column, bit 0 at the
 * top). Page 0 is Now: temperature, humidity, the air in a word, eCO2 and
 * TVOC. Then each reading charted over the last hour, day and week. Pure C,
 * host-rendered (test_airui).
 */
#include <stdbool.h>
#include <stdint.h>
#include "airlog.h"

#define AIRUI_W 128
#define AIRUI_H 64
#define AIRUI_PAGES (1 + AIR_N * AIR_RANGES)

typedef struct {
    float temp, rh;            /* NaN if no reading */
    int eco2, tvoc, aqi;       /* aqi 1..5, 0 unknown */
    int validity;              /* ENS160: 0 operating, 1 warming up, 2 first start-up */
} air_now_t;

/* The air index in a word: Excellent .. Unhealthy, or "warming up". */
const char *airui_word(const air_now_t *n);

/* The LED's colour for the air: green, through yellow and orange, to red;
   a dim blue while the sensor warms up. */
void airui_colour(const air_now_t *n, uint8_t *r, uint8_t *g, uint8_t *b);

/* Page `page` (0 .. AIRUI_PAGES-1) into fb[1024]. */
void airui_draw(uint8_t *fb, int page, const air_now_t *now, const airlog_t *log);

#endif /* BLINKY1_AIRUI_H */
