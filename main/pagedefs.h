#ifndef PAGEDEFS_H
#define PAGEDEFS_H

#include "canvas.h"
#include "pages.h"
#include "usagedata.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * What main needs to know about each page, in one table, so adding a page is
 * a view file, an enum entry and one row here -- not five edits to main.c.
 */
typedef void (*page_draw_fn)(canvas_t *c, const ud_view_t *v, float t, int64_t now_us);

/*
 * Which panels a page suits. Most of the data views are drawn for 64x20 and
 * are simply too big for a 26x7 one; a couple are written for the small panel
 * and would look lost on the large. The flag says which, rather than every
 * caller asking how wide the canvas is.
 */
#define PG_BIG    0x1        /* the CrowPanel's 64x20 */
#define PG_SMALL  0x2        /* 26x7: lilly, wave, the Feather */
#define PG_BOTH   (PG_BIG | PG_SMALL)

typedef struct {
    const char *name;        /* the menu tile */
    unsigned feeds;          /* UD_FEED(kind) for each payload that refreshes it */
    page_draw_fn draw;       /* draws from the merged view; NULL when main draws it */
    bool animated;           /* grows into place over ANIM_US when it appears */
    int64_t refresh_us;      /* redraw on its own this often; 0 never */
    unsigned char where;     /* PG_BIG, PG_SMALL or PG_BOTH */
    bool needs_touch;        /* driven by taps on its contents */
    bool in_saver;           /* shown by the cycling screensaver */
} page_def_t;

#define UD_FEED(kind) (1u << (kind))

extern const page_def_t page_defs[PAGE_COUNT];

#endif /* PAGEDEFS_H */
