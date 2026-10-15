#ifndef VAD_H
#define VAD_H

/*
 * When has the person stopped talking? Each 20 ms reading is smoothed over
 * 60 ms, and the room is measured all the time: the quietest smoothed
 * level of the last 3 s (the gaps between words reach down to it). A reading is speech when it is 2.5 times the
 * room and above 300 RMS, so a din that starts mid-turn (a fan, a TV)
 * becomes the room within the window and the turn can still end. The first
 * 300 ms only measure. After speech, 1.2 s of quiet ends it -- DONE, or
 * SILENT if under 300 ms of it was speech (a cough, a click). Nothing by
 * max_ms: SILENT; still talking at max_ms: CAPPED. Babble (other people
 * talking) is speech to it too: no energy test can tell voices apart.
 * Pure: host_tests/test_sc_vad.c.
 */
#include <stdbool.h>
#include <stdint.h>

typedef enum { VAD_WAITING, VAD_SPEAKING, VAD_DONE, VAD_SILENT, VAD_CAPPED } vad_result_t;

#define VAD_WIN 150     /* 3 s of 20 ms readings */

typedef struct {
    int fs, max_ms;
    int total_ms, speech_ms, quiet_ms;
    double floor;          /* the room, as last measured */
    bool speaking;
    float recent[3];
    int recent_i;
    float win[VAD_WIN];    /* smoothed levels, the last 3 s */
    int win_i, win_n;
} vad_t;

void vad_init(vad_t *v, int sample_rate, int max_ms);
vad_result_t vad_feed(vad_t *v, const int16_t *pcm, int samples);
int vad_speech_ms(const vad_t *v);

#endif /* VAD_H */
