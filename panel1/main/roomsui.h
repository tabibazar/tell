#ifndef ROOMSUI_H
#define ROOMSUI_H

/*
 * panel1's one page: a day of room bookings on the 480x480 panel.
 *
 *   Today   Wed 7 Oct                      19:24
 *   [PINE-1 ][PINE-2 ][CEDAR  ][MAPLE  ][BIRCH  ]   free/busy now, till when
 *    8 |       |       |Design |       |       |
 *    9 |       |       |Lena   |       |       |     a column per room,
 *   -- now -------------------------------------    hours down the side
 *
 * The hours run 08:00-16:00, stretched to take in any booking outside them.
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

/* The booking drawn under (x, y) by roomsui_draw, or NULL; its room in
   *room. Small bookings answer a little beyond their edges, for a finger. */
const room_ev_t *roomsui_hit(const rooms_day_t *day, int x, int y, int *room);

/* A card over the page, darkened, with all of booking `e` in room `r`: the
   whole title, the time, who booked it and who is invited. */
void roomsui_detail(canvas_t *c, const room_t *r, const room_ev_t *e, const roomsui_view_t *v);

/* What a room's status line says, into out, and its colour: ROOMSUI_TAKEN
   (red) or ROOMSUI_FREE (green) today, ROOMSUI_OTHER (grey) on another day.
     "Taken till 11:00, then free till 14:00"   "Taken till 15:00"
     "Free till 10:00"   "Free"   "3 bookings"   "No bookings"  */
enum { ROOMSUI_OTHER, ROOMSUI_FREE, ROOMSUI_TAKEN };
int roomsui_room_status(const room_t *r, const roomsui_view_t *v, char *out, size_t n);

/* The screen saver: the time and date, large and grey on black, at one of
   many places picked by `step` (the caller's minute count), so that nothing
   stands still on the panel for long. */
void roomsui_saver(canvas_t *c, int now, int wday, int d, int m, int step);

/* The timeline's first and last hour for `day`, as roomsui_draw picks them. */
void roomsui_hours(const rooms_day_t *day, int *first, int *last);

#endif /* ROOMSUI_H */
