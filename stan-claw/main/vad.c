#include "vad.h"

#include <math.h>
#include <string.h>

#define FLOOR_MS 300           /* the first readings only measure the room */
#define END_QUIET_MS 1200      /* older speakers pause mid-sentence: give them a breath */
#define MIN_SPEECH_MS 300
#define MIN_LEVEL 300.0
#define GATE 2.5               /* speech: this many times the room */

void vad_init(vad_t *v, int fs, int max_ms)
{
    memset(v, 0, sizeof *v);
    v->fs = fs;
    v->max_ms = max_ms;
}

/* The room's level: the quietest smoothed level in the window. Not a
   percentile: three seconds of talk are mostly voice, and any share of them
   would be voice too; the gaps between words reach down to the room. */
static float room(const vad_t *v)
{
    float m = v->win[0];
    for (int i = 1; i < v->win_n; i++) if (v->win[i] < m) m = v->win[i];
    return m;
}

vad_result_t vad_feed(vad_t *v, const int16_t *pcm, int n)
{
    if (n <= 0) return v->speaking ? VAD_SPEAKING : VAD_WAITING;
    double sum = 0;
    for (int i = 0; i < n; i++) sum += (double)pcm[i] * pcm[i];
    float rms = (float)sqrt(sum / n);
    int ms = n * 1000 / v->fs;
    v->total_ms += ms;

    /* Smoothed over the last three readings (60 ms at 20 ms frames), so a
       single spike of noise is not a word. */
    v->recent[v->recent_i++ % 3] = rms;
    int k = v->recent_i < 3 ? v->recent_i : 3;
    float level = 0;
    for (int i = 0; i < k; i++) level += v->recent[i];
    level /= k;

    /* The room, measured all the time over the last few seconds: a fan or a
       TV that starts mid-turn becomes the room within the window, so the end
       of the person's words is still found. */
    v->win[v->win_i] = level;
    v->win_i = (v->win_i + 1) % VAD_WIN;
    if (v->win_n < VAD_WIN) v->win_n++;
    v->floor = room(v);

    if (v->total_ms <= FLOOR_MS) return VAD_WAITING;
    double gate = v->floor * GATE > MIN_LEVEL ? v->floor * GATE : MIN_LEVEL;
    if (level > gate) {
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
