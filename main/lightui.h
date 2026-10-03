#ifndef LIGHTUI_H
#define LIGHTUI_H

/*
 * watch's Light page: blinky1's lamp, chosen on the wrist. BOOT steps the
 * colour (Off first), a tilt of the wrist steps the brightness, and the page
 * says whether blinky1 heard. Pure: the radio is lightlink_tx.c's, the
 * buttons and IMU main.c's. Host-rendered by test_lightui.
 */
#include <stdbool.h>
#include <stdint.h>
#include "canvas.h"

typedef struct { const char *name; uint8_t r, g, b; } light_colour_t;

#define LIGHT_N_COLOURS 8
#define LIGHT_N_LEVELS  5
extern const light_colour_t LIGHT_COLOURS[LIGHT_N_COLOURS];   /* [0] is Off */
extern const uint8_t LIGHT_LEVELS[LIGHT_N_LEVELS];             /* percent */

typedef enum { LIGHT_HEARD_UNKNOWN, LIGHT_HEARD_YES, LIGHT_HEARD_NO, LIGHT_SENDING } light_heard_t;

typedef struct {
    int colour;             /* index into LIGHT_COLOURS */
    int level;              /* index into LIGHT_LEVELS */
    light_heard_t heard;
} light_state_t;

void lightui_draw(canvas_t *c, const light_state_t *s);

#endif /* LIGHTUI_H */
