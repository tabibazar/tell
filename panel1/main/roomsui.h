#ifndef ROOMSUI_H
#define ROOMSUI_H

/*
 * panel1's one page: a day of room bookings on the 480x480 panel.
 *
 *   Today   Wed 7 Oct                      19:24
 *   [PINE-1 ][PINE-2 ][CEDAR  ][MAPLE  ][BIRCH  ]   free/busy now, till when
 *    9 |       |       |Design |       |       |
 *   10 |       |       |Lena   |       |       |     a column per room,
 *   -- now -------------------------------------    hours down the side
 *
 * The hours run 09:00-17:00, stretched to take in any booking outside them.
 * On today the passed bookings are dimmed, the one on now is lit, a red line
 * marks the minute, and each room's header is green when it is free and red
 * when it is in use, with the minute that changes. Pure: drawn on the host by
 * host_tests/test_rooms.c.
 */
#include "canvas.h"
#include "rooms.h"

typedef struct {
    int offset;          /* the shown day from today: 0 today, 1 tomorrow, -1 yesterday */
    int wday;            /* 0 Sunday .. 6 Saturday */
    int now;             /* minute of the day for the clock; -1 when not known */
    const char *note;    /* amber, beside the date: "offline since 14:05"; NULL none */
} roomsui_view_t;

/* Draws `day` (NULL: nothing fetched yet for the shown day) into `c`, which
   must be 480x480. `v->now` lights the timeline only when `v->offset` is 0. */
void roomsui_draw(canvas_t *c, const rooms_day_t *day, const roomsui_view_t *v);

/* The timeline's first and last hour for `day`, as roomsui_draw picks them. */
void roomsui_hours(const rooms_day_t *day, int *first, int *last);

#endif /* ROOMSUI_H */
