#ifndef HOTSPOT_H
#define HOTSPOT_H

/*
 * spy's 4G shared as a WiFi hotspot: a soft AP on the S3, its clients NATed
 * out through the PPP link. Off unless asked for ("hotspot on" on
 * Telegram), and off again by itself after HOTSPOT_HOURS, since the radio
 * costs the 18650 dearly. The network name and password come from the
 * gitignored secrets file, like the bot token.
 *
 * Throughput is the UART's: PPP at 921600 baud, some 0.5 Mbit/s at best.
 */
#include <stdbool.h>

#define HOTSPOT_HOURS 2

bool hotspot_configured(void);
bool hotspot_on(void);
void hotspot_off(void);
bool hotspot_is_on(void);
int hotspot_clients(void);

/* Off when its time is up; call now and then. */
void hotspot_tick(void);

#endif /* HOTSPOT_H */
