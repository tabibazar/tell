#ifndef NET_H
#define NET_H

/*
 * panel1's way out: WiFi from secrets/panel1.env, the clock from SNTP, and
 * the relay (panel1/relay/rooms.gs) over HTTPS for a day's bookings.
 */
#include <stdbool.h>
#include "rooms.h"

/* Starts WiFi and SNTP; returns at once. WiFi rejoins by itself. */
void net_start(void);

bool net_up(void);          /* joined, with an address */
bool net_time_ok(void);     /* the clock has been set */

/* Fetches y-m-d from the relay into `out`. Blocks up to ~20 s. False on any
   failure, and `out` is left as it was. */
bool net_fetch_day(int y, int m, int d, rooms_day_t *out);

/* Configured at all: a build without RELAY_URL in the secrets has nothing to ask. */
bool net_have_relay(void);

#endif /* NET_H */
