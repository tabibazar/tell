#ifndef CAMLINK_TX_H
#define CAMLINK_TX_H

/*
 * spy's side of camlink.h: the radio up at boot (a station that joins
 * nothing, on CAMLINK_CHANNEL, in modem sleep between frames), each photo
 * sent to tiny1 in chunks, and tiny1's asks passed to `on_ask`.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool camlink_start(void (*on_ask)(void));

/* Queues a photo file for tiny1 (the send runs in its own task; a tiny1 that
   does not answer the first chunk is not waited for). */
void camlink_send_file(const char *path, int hour, int minute, int weekday);

/* tiny1 answered the last send. */
bool camlink_heard(void);

#endif /* CAMLINK_TX_H */
