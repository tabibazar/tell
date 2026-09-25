#ifndef WATCH_H
#define WATCH_H

#include <stdbool.h>
#include <stdint.h>

/*
 * watch's own hardware: the Waveshare ESP32-S3-LCD-1.69 V2 (no touch) power
 * latch, its function button, its battery gauge and its buzzer line. Nothing
 * here draws; main.c asks and decides. The non-touch V2 and the Touch V2.1
 * share this pinout; the first revision of either does not.
 *
 * Only compiled into the watch build (CMakeLists excludes it elsewhere).
 */

typedef enum {
    WATCH_REV_UNKNOWN = 0,
    WATCH_REV_V21,       /* V2 (LCD) / V2.1 (Touch): SYS_EN on GPIO41 */
    WATCH_REV_OLD,       /* the first run; SYS_EN on GPIO35, an octal-PSRAM line */
} watch_rev_t;

typedef enum {
    WATCH_KEY_NONE = 0,
    WATCH_KEY_SHORT,     /* the function button, pressed and let go */
    WATCH_KEY_LONG,      /* held 2 s: reported once, while still held */
} watch_key_t;

/*
 * The first thing app_main does. Tells the revisions apart with inputs only,
 * then -- on V2.1 alone -- drives SYS_EN high so the board stays up on its
 * battery once the finger leaves the button, and holds the buzzer line low
 * (a floating buzzer base keeps the passive buzzer drawing and heats the LDO).
 */
watch_rev_t watch_power_init(void);

/* Polls the function button (SYS_OUT, GPIO40). Call every loop. */
watch_key_t watch_key_poll(int64_t now_us);

/*
 * Releases the power latch. On battery the board goes dark when the button is
 * let go and never returns from this. On USB nothing can switch it off, so
 * after a moment the latch is taken back and this returns false: the caller
 * then just darkens the screen.
 */
bool watch_power_off(void);

/* The battery, 0..100, or -1 with no reading. Averaged; cheap to call. */
int watch_battery_pct(void);

/* The battery voltage in millivolts, or -1. For the log. */
int watch_battery_mv(void);

#endif /* WATCH_H */
