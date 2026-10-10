#include "vad.h"

#include <math.h>
#include <string.h>

#define FLOOR_MS 300
#define END_QUIET_MS 800
#define MIN_SPEECH_MS 300
#define MIN_LEVEL 300.0

void vad_init(vad_t *v, int fs, int max_ms)
{
    memset(v, 0, sizeof *v);
    v->fs = fs;
    v->max_ms = max_ms;
}

vad_result_t vad_feed(vad_t *v, const int16_t *pcm, int n)
{
    if (n <= 0) return v->speaking ? VAD_SPEAKING : VAD_WAITING;
    double sum = 0;
    for (int i = 0; i < n; i++) sum += (double)pcm[i] * pcm[i];
    double rms = sqrt(sum / n);
    int ms = n * 1000 / v->fs;
    v->total_ms += ms;

    if (v->floor_ms < FLOOR_MS) {
        v->floor = (v->floor * v->floor_ms + rms * ms) / (v->floor_ms + ms);
        v->floor_ms += ms;
        return VAD_WAITING;
    }
    double gate = v->floor * 3 > MIN_LEVEL ? v->floor * 3 : MIN_LEVEL;
    if (rms > gate) {
        v->speaking = true;
        v->speech_ms += ms;
        v->quiet_ms = 0;
    } else if (v->speaking) {
        v->quiet_ms += ms;
        if (v->quiet_ms >= END_QUIET_MS) return v->speech_ms >= MIN_SPEECH_MS ? VAD_DONE : VAD_SILENT;
    }
    if (v->total_ms >= v->max_ms) return v->speaking && v->speech_ms >= MIN_SPEECH_MS ? VAD_CAPPED : VAD_SILENT;
    return v->speaking ? VAD_SPEAKING : VAD_WAITING;
}

int vad_speech_ms(const vad_t *v) { return v->speech_ms; }
