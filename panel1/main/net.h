#ifndef NET_H
#define NET_H

/*
 * panel1's way out: WiFi from the networks config.c keeps, the clock from SNTP, and
 * the relay (panel1/relay/rooms.gs) over HTTPS for a day's bookings.
 */
#include <stdbool.h>
#include "rooms.h"

/* Starts WiFi and SNTP; returns at once. Joins the strongest of the networks
   config.c knows that is in sight, and rejoins by itself. */
void net_start(void);

/* The known networks changed: choose again. */
void net_rejoin(void);

/* The network joined, "" when none. */
const char *net_ssid(void);

bool net_up(void);          /* joined, with an address */
bool net_time_ok(void);     /* the clock has been set */

/* Fetches y-m-d from the relay into `out`. Blocks up to ~35 s. False on any
   failure, and `out` is left as it was. */
bool net_fetch_day(int y, int m, int d, rooms_day_t *out);

/* A relay URL is set (config.c): without one there is nothing to ask. */
bool net_have_relay(void);

#endif /* NET_H */
