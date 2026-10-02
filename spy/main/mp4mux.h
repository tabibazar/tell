#ifndef MP4MUX_H
#define MP4MUX_H

/*
 * A minimal MP4 writer for one H.264 video track, for spy's hourly clips.
 *
 * Frames go in as the encoder hands them out: Annex-B (start-code separated
 * NAL units), one access unit per call, the first an IDR carrying the SPS and
 * PPS. The SPS and PPS are taken out into the track's avcC box; every frame's
 * NALs are written to mdat length-prefixed (AVCC), as MP4 wants. On close the
 * moov box goes at the end, after mdat, with the sample table -- sizes, the
 * key frames, one chunk, a constant frame duration.
 *
 * Espressif's esp_muxer would not take the ESP32-S3 software encoder's
 * SPS/PPS ("MP4 fail to parse SPS-PPS"), so this is ours. Pure C over a
 * FILE*, so it runs and is tested on the host.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct mp4mux mp4mux_t;

/* Starts a file at `f` (opened "wb+", positioned at 0). NULL on no memory. */
mp4mux_t *mp4mux_open(FILE *f, uint16_t width, uint16_t height, uint32_t fps);

/* One access unit, Annex-B. `key` marks an IDR. False on a write failure or a
   frame that carries no picture. The first frame must be an IDR with SPS/PPS. */
bool mp4mux_add(mp4mux_t *m, const uint8_t *annexb, size_t len, bool key);

/* Writes moov, patches mdat's size and frees `m` (the FILE stays open).
   False if anything failed or no frame was added. */
bool mp4mux_close(mp4mux_t *m);

#endif /* MP4MUX_H */
