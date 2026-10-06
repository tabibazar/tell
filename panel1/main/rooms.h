#ifndef ROOMS_H
#define ROOMS_H

/*
 * One day of meeting-room bookings, as the relay (panel1/relay/rooms.gs)
 * sends it: rooms in name order, each with its bookings as minutes from local
 * midnight. Pure: parsed and checked on the host (host_tests/test_rooms.c).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROOMS_MAX 6
#define ROOM_EV_MAX 24

typedef struct {
    int16_t start, end;      /* minutes from midnight, 0..1440, start < end */
    char title[56];          /* ASCII only: the panel's faces have nothing else */
    char who[24];            /* the organiser's first name */
} room_ev_t;

typedef struct {
    char name[20];           /* "CEDAR", upper case, capacity split off */
    int cap;                 /* seats, from "CEDAR (8)"; 0 when not given */
    int n;
    room_ev_t ev[ROOM_EV_MAX];  /* by start time */
} room_t;

typedef struct {
    int y, m, d;             /* the day, local */
    int nrooms;
    room_t room[ROOMS_MAX];
} rooms_day_t;

/* Parses the relay's JSON into `out`. False (and `out` untouched) when it is
   not the relay's answer: an error page, an {"err":...}, no rooms. */
bool rooms_parse(const char *json, size_t len, rooms_day_t *out);

/* The booking in room `r` at minute `now`, or NULL. */
const room_ev_t *rooms_at(const room_t *r, int now);

/* The next booking in room `r` starting after `now`, or NULL. */
const room_ev_t *rooms_next(const room_t *r, int now);

#endif /* ROOMS_H */
