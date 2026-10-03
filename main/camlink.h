#ifndef CAMLINK_H
#define CAMLINK_H

/*
 * spy -> tiny1: spy's photos on tiny1's screen, over ESP-NOW. The second
 * board-to-board link (Reza, 2026-10-03, after watch -> blinky1).
 *
 * A JPEG goes as chunks of up to CAMLINK_CHUNK bytes, each carrying the
 * frame's number, its whole length, the chunk's offset and the time the
 * photo was taken; tiny1 shows a frame only once every byte has arrived.
 * tiny1 sends CAMLINK_ASK for a fresh photo (its BOOT button). Channel 6, the
 * channel spy's hotspot uses, so the two share the radio. Each side listens
 * only to the other's MAC. Shared by spy/ and tiny1/: keep it plain C.
 */
#include <stdint.h>

#define CAMLINK_CHANNEL 6
#define CAMLINK_MAGIC   0x4D41434Bu      /* "KCAM" */
#define CAMLINK_CHUNK   1400             /* ESP-NOW v2 carries up to 1470 */
#define CAMLINK_MAX     (64 * 1024)      /* a frame's JPEG, at most */

#define CAMLINK_SPY_MAC   { 0x28, 0x84, 0x85, 0x91, 0x87, 0xc4 }
#define CAMLINK_TINY1_MAC { 0x68, 0xee, 0x8f, 0xda, 0x6c, 0x78 }

enum { CAMLINK_PART = 1, CAMLINK_ASK = 2 };

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  type;           /* CAMLINK_PART or CAMLINK_ASK */
    uint8_t  hour, minute;   /* when the photo was taken (local) */
    uint8_t  weekday;        /* 0 Sunday; 0xFF no clock */
    uint16_t frame;          /* which photo */
    uint16_t len;            /* this chunk's bytes */
    uint32_t total;          /* the whole JPEG */
    uint32_t offset;         /* where this chunk goes */
    /* then `len` bytes of JPEG */
} camlink_hdr_t;

#endif /* CAMLINK_H */
