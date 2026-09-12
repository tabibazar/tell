#ifndef WIFI_H
#define WIFI_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * The radio as a listening instrument, not as a way onto a network.
 *
 * It never joins anything, which is the point rather than a limitation: a
 * scan needs no credentials, no password and no permission, so the board
 * works in a cafe or someone else's house exactly as it does at home. It also
 * sidesteps what the README objected to in the first place -- "WiFi means
 * credentials baked into firmware and a board that stops working when the
 * network changes". There are no credentials here to bake.
 *
 * What it is for: carrying to where the question is. A laptop can tell you
 * the signal where the laptop is; this has a battery and a screen and can be
 * held up in the far corner of a room.
 *
 * The radio is 2.4 GHz only, so 5 GHz networks are invisible to it. That is
 * the silicon, not the configuration, and it means a survey done with this
 * covers one of the two bands.
 */

/* Brings the radio up in listening mode. */
esp_err_t wifi_start(void);

/* What a scan found: how strong, and on which channel. */
typedef struct {
    char    ssid[33];
    int8_t  rssi;        /* dBm; -30 is next to it, -90 is barely there */
    uint8_t channel;
} wifi_ap_t;

/* Scans every channel and fills `out`, strongest first. Returns how many were
   written. Blocks for a second or two. */
int wifi_scan(wifi_ap_t *out, int max);

#endif /* WIFI_H */
