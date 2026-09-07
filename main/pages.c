#include "pages.h"

void pages_init(pages_t *p, unsigned available)
{
    /* The clock is the fallback, so it must always exist. */
    p->available = available | PAGE_BIT(PAGE_CLOCK);
    p->current = PAGE_CLOCK;
    p->last_activity_us = 0;
    p->last_rotate_us = 0;
}

page_t pages_advance(pages_t *p, int64_t now_us)
{
    for (int i = 1; i <= PAGE_COUNT; i++) {
        page_t candidate = (page_t)(((int)p->current + i) % PAGE_COUNT);
        if (p->available & PAGE_BIT(candidate)) {
            p->current = candidate;
            break;
        }
    }
    p->last_activity_us = now_us;
    p->last_rotate_us = now_us;
    return p->current;
}

void pages_show(pages_t *p, page_t page, int64_t now_us)
{
    if (!(p->available & PAGE_BIT(page))) return;
    p->current = page;
    p->last_activity_us = now_us;
    p->last_rotate_us = now_us;
}

bool pages_idle_expired(const pages_t *p, int64_t now_us)
{
    if (p->current == PAGE_CLOCK) return false;
    return now_us - p->last_activity_us > PAGES_IDLE_US;
}

bool pages_tick(pages_t *p, int64_t now_us)
{
    if (PAGES_ROTATE_US <= 0) return false;    /* rotation disabled */

    /* While a page is pinned by a touch or a message, leave it alone. */
    if (now_us - p->last_activity_us <= PAGES_IDLE_US) return false;
    if (now_us - p->last_rotate_us < PAGES_ROTATE_US) return false;

    page_t before = p->current;
    for (int i = 1; i <= PAGE_COUNT; i++) {
        page_t candidate = (page_t)(((int)p->current + i) % PAGE_COUNT);
        if (p->available & PAGE_BIT(candidate)) {
            p->current = candidate;
            break;
        }
    }
    /* Rotation must not count as activity, or it would pin the page it just
       moved to and stop rotating. */
    p->last_rotate_us = now_us;
    return p->current != before;
}
