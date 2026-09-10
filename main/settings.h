#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The few things worth changing from the screen itself, kept across power
 * cycles. The option tables live here too, so the page that draws them and
 * the hit-test that reads taps agree on what the choices are.
 */
typedef struct {
    int saver_min;       /* minutes idle before the screensaver; 0 = never */
    bool saver_cycle;    /* true: cycle the pages; false: drift the clock */
    int dwell_s;         /* seconds each page stays while cycling */
    bool auto_now;       /* jump to the live page while Claude is busy */
    bool home_now;       /* the page the board rests on: today, live rather than the clock */
} settings_t;

#define SETTINGS_ROWS 5
#define SETTINGS_MAX_CHOICES 7

typedef struct {
    const char *label;
    const char *choices[SETTINGS_MAX_CHOICES];
    int count;
} settings_row_t;

void settings_defaults(settings_t *s);

/* The nth setting's label and choice names, NULL past the end. */
const settings_row_t *settings_row(int row);

/* Which choice a setting currently has. */
int settings_choice(const settings_t *s, int row);

/* Applies a choice. True if anything changed; out-of-range is ignored. */
bool settings_select(settings_t *s, int row, int choice);

/* Persistence. On the host these are no-ops: load leaves the defaults. */
void settings_load(settings_t *s);
bool settings_save(const settings_t *s);

/* The time zone the Mac last reported, kept so the title bars can show UTC
   from the RTC's time straight after a power cycle, before any Mac speaks.
   Not a setting anyone chooses, but the same flash and the same rules. */
bool settings_load_zone(int *utc_offset_min, char *tz, int tz_size);
bool settings_save_zone(int utc_offset_min, const char *tz);

/* The pages the cycling screensaver has been told to leave out, as a bit per
   page. Chosen by double-tapping a tile on the menu rather than from the
   settings page: there are twenty pages and the menu already shows them all,
   where a settings row per page would be a screen of its own. */
bool settings_load_cycle_off(uint32_t *mask);
bool settings_save_cycle_off(uint32_t mask);

#endif /* SETTINGS_H */
