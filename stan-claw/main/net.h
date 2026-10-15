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

/* WiFi setup from a phone: a second network of its own (WPA2, `pass`) beside
   the home one, and back. While it runs the home side keeps trying. */
void net_setup_start(const char *ssid, const char *pass);
void net_setup_stop(void);
/* The networks last seen, strongest first, each once; a scan if none yet. */
int net_seen(char names[][33], int max);
/* No known network is in sight (or none is known), or the one in sight has
   refused us three times running (its password changed): time to offer setup. */
bool net_none_known(void);
/* Join this one next, ahead of stronger known ones (it was just set up). */
void net_prefer(const char *ssid);
bool net_time_ok(void);     /* the clock has been set */

#endif /* NET_H */
