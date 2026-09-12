#ifndef PAGES_H
#define PAGES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    /* Tap order. The clock is home; the pages you glance at most come first,
       and the two text-heavy ones sit last, just before the clock returns. */
    PAGE_CLOCK = 0,
    PAGE_MENU,       /* a tile per page; first tap from the clock opens it */
    PAGE_NOW,        /* today so far, and whether Claude is busy */
    PAGE_STORY,      /* a recap of the day, written by Claude */
    PAGE_LIMITS,     /* session and weekly limits, counting down */
    PAGE_STATS,
    PAGE_TODAY,
    PAGE_MODELS,     /* tokens per day, one line per model */
    PAGE_PROJECTS,   /* tokens by repository */
    PAGE_YEAR,       /* the last twelve months as a heatmap */
    PAGE_RHYTHM,     /* messages by weekday and hour */
    PAGE_COST,       /* what it would have cost on the API */
    PAGE_CACHE,      /* prompt-cache hit rate and savings */
    PAGE_TOOLS,      /* which tools Claude calls */
    PAGE_RUNS,       /* the programs behind the Bash calls */
    PAGE_THINKING,   /* thinking versus visible output */
    PAGE_RECORDS,    /* personal bests */
    PAGE_TURNS,      /* how long Claude takes */
    PAGE_MESSAGE,
    PAGE_DAILY,      /* tokens per day as bars */
    PAGE_PARTICLES,  /* tilt-poured sand; the Feather and wave, which have IMUs */
    PAGE_TIMER,      /* a countdown you shake to restart */
    PAGE_STOPWATCH,  /* counting up; shake to start and stop */
    PAGE_TEMPS,      /* die and crystal, charted over time */
    PAGE_WIFI,       /* what is on the air, and how loud */
    PAGE_RTC,        /* the clock chip, and how far the board has drifted */
    PAGE_LEVEL,      /* the Feather's spirit level; needs its IMU */
    PAGE_USAGE,      /* all-time totals, drawn for a small panel */
    PAGE_SETTINGS,   /* touch-only; last, so it is out of the way */
    PAGE_COUNT
} page_t;

#define PAGE_BIT(p) (1u << (p))

/* How long a page stays pinned after a touch or a new message. */
#define PAGES_IDLE_US (5 * 60 * 1000000LL)

/* The default rotation interval. Zero disables rotation, leaving the display
   wherever it was last put; a board sets its own with pages_set_rotate.
   Rotation exists for two reasons: a static image on an LCD risks retention,
   and a board with no touch and two buttons is better at showing you things
   in turn than at being navigated. */
#define PAGES_ROTATE_US 0

/* How long a touch, a button or a message holds the rotation off. Shorter
   than PAGES_IDLE_US, which pins a page against the screensaver: a board that
   rotates is meant to keep moving, and a minute is long enough to read what
   you pressed a button to see. */
#define PAGES_ROTATE_PIN_US (60 * 1000000LL)

/* After this long with no touch and no message, the screensaver takes over,
   either cycling the pages or drifting the clock, so nothing sits still long
   enough to burn in. This is the default; the Settings page changes it at
   run time through pages_set_saver. */
#define PAGES_SAVER_US (10 * 60 * 1000000LL)

typedef struct {
    unsigned available;      /* bitmask of PAGE_BIT(...) */
    page_t current;
    int64_t last_activity_us;
    int64_t last_rotate_us;
    int64_t saver_us;        /* idle time before the saver; 0 disables it */
    int64_t rotate_us;       /* time on each page once idle; 0 disables it */
} pages_t;

void pages_init(pages_t *p, unsigned available);

/* Changes the idle time before the screensaver. Zero means never. */
void pages_set_saver(pages_t *p, int64_t us);

/* How long each page gets before the next one. Zero leaves the display where
   it was last put, which is the default. A board that rotates needs no
   screensaver -- nothing on it stays still long enough to burn in -- so this
   turns the saver off, and pages_saver_active says so. */
void pages_set_rotate(pages_t *p, int64_t us);

/* Moves to the next available page not in `skip`, without counting as
   activity, so a slideshow can step through the pages while the saver stays
   active. Returns the page it landed on, unchanged if there is nowhere to go. */
page_t pages_step(pages_t *p, unsigned skip);

/* Moves to the next available page and returns it. */
page_t pages_advance(pages_t *p, int64_t now_us);

/* Moves to the previous available page and returns it. A tap on the left
   half of the screen; the right half advances. */
page_t pages_back(pages_t *p, int64_t now_us);

/* Jumps to a page, ignored if that page is not available on this board. */
void pages_show(pages_t *p, page_t page, int64_t now_us);

/* True when the page was pinned by a touch or message and that has expired. */
bool pages_idle_expired(const pages_t *p, int64_t now_us);

/* True when the screensaver should be showing. */
bool pages_saver_active(const pages_t *p, int64_t now_us);

/* Call every loop. Once nothing has been pinned for PAGES_ROTATE_PIN_US,
   advances to the next page every rotate_us, leaving out `skip` -- the same
   mask the screensaver uses, so a page struck off the round stays off it
   here too. Returns true if the page changed. */
bool pages_tick(pages_t *p, int64_t now_us, unsigned skip);

#endif /* PAGES_H */
