#include "pages.h"

void pages_init(pages_t *p, unsigned available)
{
    /* The clock is the fallback, so it must always exist. */
    p->available = available | PAGE_BIT(PAGE_CLOCK);
    p->current = PAGE_CLOCK;
    p->last_activity_us = 0;
    p->last_rotate_us = 0;
    p->saver_us = PAGES_SAVER_US;
    p->rotate_us = PAGES_ROTATE_US;
}

void pages_set_saver(pages_t *p, int64_t us)
{
    p->saver_us = us < 0 ? 0 : us;
}

void pages_set_rotate(pages_t *p, int64_t us)
{
    p->rotate_us = us < 0 ? 0 : us;
}

page_t pages_step(pages_t *p, unsigned skip)
{
    for (int i = 1; i <= PAGE_COUNT; i++) {
        page_t candidate = (page_t)(((int)p->current + i) % PAGE_COUNT);
        if ((p->available & PAGE_BIT(candidate)) && !(skip & PAGE_BIT(candidate))) {
            p->current = candidate;
            break;
        }
    }
    return p->current;
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

page_t pages_back(pages_t *p, int64_t now_us)
{
    for (int i = 1; i <= PAGE_COUNT; i++) {
        page_t candidate = (page_t)(((int)p->current - i + PAGE_COUNT) % PAGE_COUNT);
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

bool pages_saver_active(const pages_t *p, int64_t now_us)
{
    /* A rotating board is its own screensaver, and a better one: the pages
       are changing anyway, so nothing burns in, and the saver's own slideshow
       on top of the rotation would be two clocks fighting over the timing. */
    if (p->rotate_us > 0) return false;
    if (p->saver_us <= 0) return false;
    return now_us - p->last_activity_us > p->saver_us;
}

bool pages_tick(pages_t *p, int64_t now_us, unsigned skip)
{
    if (p->rotate_us <= 0) return false;       /* rotation disabled */

    /* While a page is pinned by a touch, a button or a message, leave it
       alone -- but for a minute, not the five that hold the saver off. The
       board is meant to keep moving. */
    if (now_us - p->last_activity_us <= PAGES_ROTATE_PIN_US) return false;
    if (now_us - p->last_rotate_us < p->rotate_us) return false;

    page_t before = p->current;
    pages_step(p, skip);
    /* Rotation must not count as activity, or it would pin the page it just
       moved to and stop rotating. */
    p->last_rotate_us = now_us;
    return p->current != before;
}
