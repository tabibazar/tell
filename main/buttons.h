#ifndef BUTTONS_H
#define BUTTONS_H

#include "esp_err.h"

/*
 * The two push buttons on the T-Display-S3. They take the place of the taps
 * the big board gets from its touch panel, so they report the same two
 * intentions and main routes them to the same page calls.
 */
typedef enum {
    BUTTON_NONE = 0,
    BUTTON_PREV,        /* GPIO0, the BOOT button -- like a tap on the left */
    BUTTON_NEXT,        /* GPIO14 -- like a tap on the right */
} button_t;

esp_err_t buttons_init(void);

/* The next press since the last call, or BUTTON_NONE. Edge triggered, so
   holding a button reports once; poll it on the main tick. */
button_t buttons_pressed(void);

#endif /* BUTTONS_H */
