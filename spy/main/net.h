#ifndef NET_H
#define NET_H

/*
 * spy's link to the internet: the SIM7670G over UART1 (GPIO17 RX, GPIO18 TX,
 * 115200), PPP through esp_modem, then the time from SNTP.
 *
 * The modem is powered by GPIO21 high (or the "4G" DIP; either does) and
 * starts by itself when power appears -- PWRKEY is tied, there is no pulse to
 * give (docs/hardware/spy-pinout.md). The APN is Freedom Mobile's, read from
 * the modem's own context 1 on 2026-10-01.
 */
#include <stdbool.h>
#include <stdint.h>

/* Powers the modem, brings PPP up and sets the clock. Blocks up to ~2 min.
   Safe to call again: it tears down what is there and starts over. */
bool net_up(void);

/* PPP has an address. */
bool net_ok(void);

/* The system clock has been set (from the modem's network time or SNTP). */
bool net_time_ok(void);

/* The signal as AT+CSQ gave it at the last net_up (0-31, 99 unknown). */
int net_csq(void);

typedef struct { double lat, lon, alt; int secs; } net_fix_t;

/* A position from the modem's own GNSS: PPP pauses, the receiver is
   switched on and asked every 3 s for up to `max_s`, then switched off and
   PPP redialled. Needs the ceramic antenna on the GNSS socket and some sky. */
bool net_gps(int max_s, net_fix_t *fix);

#endif /* NET_H */
