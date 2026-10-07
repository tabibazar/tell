#ifndef CONSOLE_H
#define CONSOLE_H

/*
 * panel1's settings over its USB serial (115200), a line at a time:
 *
 *   wifi add NAME PASSWORD     a network to know (quote a name with spaces:
 *                              wifi add "Office WiFi" secret); again to
 *                              change its password
 *   wifi forget NAME           forget one
 *   wifi list                  the networks known (passwords not shown)
 *   relay URL                  the relay's /exec URL
 *   status                     what it is on and what it last fetched
 *
 * Kept in NVS (config.c), so they stay through restarts and reflashing.
 */
void console_start(void);

/* panel1.c: drop every fetched day, so the next ones come from the new relay. */
void panel1_refetch(void);

#endif /* CONSOLE_H */
