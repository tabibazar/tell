#ifndef NET_H
#define NET_H

/*
 * panel1's way out: WiFi from the networks config.c keeps, the clock from SNTP,
 * for stan-claw.
 */
#include <stdbool.h>

/* Starts WiFi and SNTP; returns at once. Joins the strongest of the networks
   config.c knows that is in sight, and rejoins by itself. */
void net_start(void);

/* The known networks changed: choose again. */
void net_rejoin(void);

/* The network joined, "" when none. */
const char *net_ssid(void);

bool net_up(void);          /* joined, with an address */
bool net_time_ok(void);     /* the clock has been set */

#endif /* NET_H */
