#ifndef PAGES_H
#define PAGES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PAGE_CLOCK = 0,
    PAGE_STATS,
    PAGE_DAILY,
    PAGE_MESSAGE,
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

/* After this long with no touch and no message, the screensaver takes over:
   a drifting clock, so nothing sits still long enough to burn in. */
#define PAGES_SAVER_US (5 * 60 * 1000000LL)

typedef struct {
    unsigned available;      /* bitmask of PAGE_BIT(...) */
    page_t current;
    int64_t last_activity_us;
    int64_t last_rotate_us;
} pages_t;

void pages_init(pages_t *p, unsigned available);

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
