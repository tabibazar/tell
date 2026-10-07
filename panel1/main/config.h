#ifndef CONFIG_H
#define CONFIG_H

/*
 * panel1's settings, kept in NVS so they are changed over USB without a
 * rebuild and survive reflashing: the WiFi networks it knows and the relay's
 * URL. The first boot seeds them from secrets/panel1.env as built in; after
 * that the console (console.c) owns them. Safe from any task.
 */
#include <stdbool.h>
#include <stddef.h>

#define CFG_NETS 6

typedef struct {
    char ssid[33];
    char pass[65];
} cfg_net_t;

void cfg_load(void);

/* Copies the known networks into out[CFG_NETS]; returns how many. */
int cfg_nets(cfg_net_t *out);

/* Adds a network, or changes the password of one already known. False when
   the list is full or the name is empty. */
bool cfg_net_add(const char *ssid, const char *pass);

/* Forgets a network; false if it was not known. */
bool cfg_net_forget(const char *ssid);

/* The relay's URL, copied into buf; "" when none. */
void cfg_relay(char *buf, size_t n);
void cfg_set_relay(const char *url);

#endif /* CONFIG_H */
