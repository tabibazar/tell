#ifndef ROOMSUI_H
#define ROOMSUI_H

/*
 * panel1's pages: a day of room bookings on the 480x480 panel.
 *
 * The overview, a card per room, two across:
 *
 *   Today   Thu 8 Oct                      10:20
 *   +--------------------+ +--------------------+
 *   | Grande             | | Ristretto          |   green when free now,
 *   | Free till 11:00    | | Busy till 10:45    |   red when in use
 *   | Next: Design review| | Hiring sync        |
 *   | 11:00 - Omar Haddad| | Nora Ali           |
 *   +--------------------+ +--------------------+
 *
 * Another day the cards are grey, with how many bookings and the seats. A tap
 * on a card opens that room's own page: its bookings on a timeline, full
 * width, with their titles and who booked them, and a status strip ("Taken
 * till 10:45, then free till 14:00").
 *
 * The hours run 08:00-16:00, stretched to take in any booking outside them.
 * On today the passed bookings are dimmed, the one on now is lit and a red
 * line marks the minute. Pure: drawn on the host by host_tests/test_rooms.c.
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

/* A card over the page, darkened, with all of booking `e` in room `r`: the
   whole title, the time, who booked it and who is invited. */
void roomsui_detail(canvas_t *c, const room_t *r, const room_ev_t *e, const roomsui_view_t *v);

/* What a room's status line says, into out, and its colour: ROOMSUI_TAKEN
   (red) or ROOMSUI_FREE (green) today, ROOMSUI_OTHER (grey) on another day.
     "Taken till 11:00, then free till 14:00"   "Taken till 15:00"
     "Free till 10:00"   "Free"   "3 bookings"   "No bookings"  */
enum { ROOMSUI_OTHER, ROOMSUI_FREE, ROOMSUI_TAKEN };
int roomsui_room_status(const room_t *r, const roomsui_view_t *v, char *out, size_t n);

/* One room's page: its name, seats and day, a status strip, and its bookings
   full width on the same hours as the overview, a dot per room at the foot.
   With no day, or no such room, it draws the overview instead. */
void roomsui_room_draw(canvas_t *c, const rooms_day_t *day, int room, const roomsui_view_t *v);

/* The room whose card on the overview is under (x, y), or -1. */
int roomsui_head_hit(const rooms_day_t *day, int x, int y);

/* The booking under (x, y) on room `room`'s page, or NULL. */
const room_ev_t *roomsui_room_hit(const rooms_day_t *day, int room, int x, int y);

/* Is (x, y) in a room page's header or status strip: the way back? */
bool roomsui_room_head_hit(int x, int y);

/* The timeline's first and last hour for `day`, as roomsui_draw picks them. */
void roomsui_hours(const rooms_day_t *day, int *first, int *last);

#endif /* ROOMSUI_H */
