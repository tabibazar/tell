#ifndef STAMP_H
#define STAMP_H

/* The time ("07:42") burnt into the bottom-right corner of an I420 frame,
   white digits on a dark box, so a time-lapse says when each frame was. */
#include <stdint.h>

void stamp_time(uint8_t *i420, int w, int h, int hour, int minute);

#endif /* STAMP_H */
