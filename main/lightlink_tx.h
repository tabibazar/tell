#ifndef LIGHTLINK_TX_H
#define LIGHTLINK_TX_H

/*
 * watch's side of the watch -> blinky1 link (lightlink.h): WiFi up only while
 * the Light page is showing, ESP-NOW unicast to blinky1, and whether the last
 * message was acknowledged. watch build only.
 */
#include <stdbool.h>
#include <stdint.h>
#include "lightui.h"

void lightlink_radio(bool on);
bool lightlink_send(bool on, uint8_t r, uint8_t g, uint8_t b, uint8_t level);

/* The last send's outcome: LIGHT_SENDING until the MAC-level ack (or its
   absence) comes back. */
light_heard_t lightlink_heard(void);

#endif /* LIGHTLINK_TX_H */
