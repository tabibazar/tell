#include "view_common.h"

#include <stdio.h>
#include <string.h>
#include "pagedefs.h"
#include "pages.h"

/* The menu: tiles four across, five down, one per available page, in tap
   order. Each is a raised plate the size of a fingertip several times over. */
#define MENU_COLS   4
#define MENU_TOP    30
#define MENU_PITCH  88
#define MENU_TILE_H 80

static const char *menu_name(page_t page)
{
    return page_defs[page].name ? page_defs[page].name : "?";
}

/* The nth tile's page, walking the available pages in order, skipping the
   menu itself. PAGE_COUNT when there is no nth tile. */
static page_t menu_tile_page(const pages_t *p, int n)
{
    int seen = 0;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (i == PAGE_MENU || !(p->available & PAGE_BIT(i))) continue;
        if (seen++ == n) return (page_t)i;
    }
    return PAGE_COUNT;
}

static void menu_tile_rect(canvas_t *c, int n, int *x, int *y, int *w, int *h)
{
    int tile_w = c->w / MENU_COLS;
    *x = (n % MENU_COLS) * tile_w + 4;
    *y = MENU_TOP + (n / MENU_COLS) * MENU_PITCH;
    *w = tile_w - 8;
    *h = MENU_TILE_H;
}

void views_menu(canvas_t *c, const pages_t *p)
{
    canvas_clear(c);
    vw_title(c, "MENU", "tap a page");
    for (int n = 0; ; n++) {
        page_t page = menu_tile_page(p, n);
        if (page == PAGE_COUNT) break;
        int x, y, w, h;
        menu_tile_rect(c, n, &x, &y, &w, &h);
        if (y + h > c->h) break;
        canvas_fill_rect(c, x, y, w, h, 0x2124);
        canvas_fill_rect(c, x, y, w, 3, 0x4208);
        canvas_fill_rect(c, x, y + h, w, 2, 0x18C3);
        const char *name = menu_name(page);
        int tw = (int)strlen(name) * c->cell_w;
        canvas_puts_px(c, x + (w - tw) / 2, y + (h - c->cell_h) / 2, name,
                       page == p->current ? pal_heat(PAL_HEAT_STEPS) : PAL_FG);
    }
}

bool views_menu_hit(canvas_t *c, const pages_t *p, int px, int py, page_t *page)
{
    for (int n = 0; ; n++) {
        page_t candidate = menu_tile_page(p, n);
        if (candidate == PAGE_COUNT) return false;
        int x, y, w, h;
        menu_tile_rect(c, n, &x, &y, &w, &h);
        /* The gaps count too: a finger between two tiles meant one of them. */
        if (px >= x - 4 && px < x + w + 4 && py >= y - 4 && py < y + MENU_PITCH - 4) {
            *page = candidate;
            return true;
        }
    }
}
