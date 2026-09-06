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

/* A page other than the clock reverts to the clock after this long with no
   touch and no new data. */
#define PAGES_IDLE_US (5 * 60 * 1000000LL)

typedef struct {
    unsigned available;      /* bitmask of PAGE_BIT(...) */
    page_t current;
    int64_t last_activity_us;
} pages_t;

void pages_init(pages_t *p, unsigned available);

/* Moves to the next available page and returns it. */
page_t pages_advance(pages_t *p, int64_t now_us);

/* Jumps to a page, ignored if that page is not available on this board. */
void pages_show(pages_t *p, page_t page, int64_t now_us);

/* True when the clock should take over. Always false on the clock itself. */
bool pages_idle_expired(const pages_t *p, int64_t now_us);

#endif /* PAGES_H */
