#ifndef SPY_CLIP_H
#define SPY_CLIP_H

/*
 * An hour's time-lapse frames into an MP4: each /sdcard/tl/<day>/HHMM.jpg of
 * the hour, in order, decoded (esp_jpeg), turned to I420 with the time
 * stamped in a corner, encoded by esp_h264's software encoder at 640x480 and
 * written by mp4mux at 10 fps -- an hour's 60 frames make six seconds.
 *
 * About 1.2 s a frame on one core: a full hour takes a minute and a bit.
 */
#include <stdbool.h>

#define CLIP_W   640
#define CLIP_H   480
#define CLIP_FPS 10

/* `day_dir` like "/sdcard/tl/20261002", `hour` 0-23, `last_min` the last
   minute to include (59 for a whole hour; fewer for "clip" mid-hour).
   Writes `out`; `*frames` gets how many went in. False if none did. */
bool clip_make(const char *day_dir, int hour, int last_min, const char *out, int *frames);

#endif /* SPY_CLIP_H */
