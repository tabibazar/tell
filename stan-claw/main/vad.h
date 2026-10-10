#ifndef VAD_H
#define VAD_H

/*
 * When has the person stopped talking? The first 300 ms measure the room;
 * a frame is speech when it is three times the room and above 300 RMS.
 * After speech, 800 ms of quiet ends it -- DONE, or SILENT if under 300 ms
 * of it was speech (a cough, a click). Nothing by max_ms: SILENT; still
 * talking at max_ms: CAPPED. Pure: host_tests/test_sc_vad.c.
 */
#include <stdbool.h>
#include <stdint.h>

typedef enum { VAD_WAITING, VAD_SPEAKING, VAD_DONE, VAD_SILENT, VAD_CAPPED } vad_result_t;

typedef struct {
    int fs, max_ms;
    int total_ms, speech_ms, quiet_ms, floor_ms;
    double floor;
    bool speaking;
} vad_t;

void vad_init(vad_t *v, int sample_rate, int max_ms);
vad_result_t vad_feed(vad_t *v, const int16_t *pcm, int samples);
int vad_speech_ms(const vad_t *v);

#endif /* VAD_H */
