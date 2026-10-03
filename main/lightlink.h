#ifndef LIGHTLINK_H
#define LIGHTLINK_H

/*
 * watch -> blinky1: the one radio link between two boards. Everything else in
 * the fleet is a BLE peripheral the Mac drives; on 2026-10-02 Reza chose a
 * direct link for this pair, so the light switch works with no Mac awake.
 *
 * ESP-NOW, unicast from watch's station MAC to blinky1's, on a fixed channel
 * (neither joins a WiFi network). Each message carries the whole state, so a
 * lost or repeated one does no harm; blinky1 ignores any sender but watch.
 * Shared by the screen project (watch) and blinky1/ -- keep it plain C.
 */
#include <stdint.h>

#define LIGHT_CHANNEL 1
#define LIGHT_MAGIC   0x4B4E4C42u      /* "BLNK" */
#define LIGHT_VERSION 1

#define LIGHT_WATCH_MAC   { 0x80, 0x45, 0x6b, 0x35, 0x11, 0xd4 }
#define LIGHT_BLINKY1_MAC { 0x10, 0x51, 0xdb, 0x79, 0xb3, 0xa8 }

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  on;            /* 0: dark, whatever the colour */
    uint8_t  r, g, b;       /* the colour at full strength */
    uint8_t  level;         /* brightness, 1..100 % */
    uint16_t seq;           /* for the log only */
} light_msg_t;

#endif /* LIGHTLINK_H */
