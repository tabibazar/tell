#ifndef PAGES_H
#define PAGES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    /* Tap order. The clock is home; the pages you glance at most come first,
       and the two text-heavy ones sit last, just before the clock returns. */
    PAGE_CLOCK = 0,
    PAGE_NOW,        /* today so far, and whether Claude is busy */
    PAGE_STATS,
    PAGE_TODAY,
    PAGE_MODELS,     /* tokens per day, one line per model */
    PAGE_PROJECTS,   /* tokens by repository */
    PAGE_YEAR,       /* the last twelve months as a heatmap */
    PAGE_RHYTHM,     /* messages by weekday and hour */
    PAGE_COST,       /* what it would have cost on the API */
    PAGE_CACHE,      /* prompt-cache hit rate and savings */
    PAGE_MESSAGE,
    PAGE_DAILY,      /* tokens per day as bars */
    PAGE_SETTINGS,   /* touch-only; last, so it is out of the way */
    PAGE_COUNT
} page_t;

#define PAGE_BIT(p) (1u << (p))

/* How long a page stays pinned after a touch or a new message. */
#define PAGES_IDLE_US (5 * 60 * 1000000LL)

/* Once idle, pages advance this often. Zero disables rotation, leaving the
   display wherever it was last put. Rotation exists because a static image
   on an LCD risks retention, so turning it off means the clock can sit
   unchanged for hours. */
#define PAGES_ROTATE_US 0

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
} pages_t;

void pages_init(pages_t *p, unsigned available);

/* Changes the idle time before the screensaver. Zero means never. */
void pages_set_saver(pages_t *p, int64_t us);

/* Moves to the next available page not in `skip`, without counting as
   activity, so a slideshow can step through the pages while the saver stays
   active. Returns the page it landed on, unchanged if there is nowhere to go. */
page_t pages_step(pages_t *p, unsigned skip);

/* Moves to the next available page and returns it. */
page_t pages_advance(pages_t *p, int64_t now_us);

/* Jumps to a page, ignored if that page is not available on this board. */
void pages_show(pages_t *p, page_t page, int64_t now_us);

/* True when the page was pinned by a touch or message and that has expired. */
bool pages_idle_expired(const pages_t *p, int64_t now_us);

/* True when the screensaver should be showing. */
bool pages_saver_active(const pages_t *p, int64_t now_us);

/* Call every loop. Once nothing has been pinned for PAGES_IDLE_US, advances
   to the next page every PAGES_ROTATE_US. Returns true if the page changed. */
bool pages_tick(pages_t *p, int64_t now_us);

#endif /* PAGES_H */
