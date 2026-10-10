#ifndef AUDIO_H
#define AUDIO_H

/*
 * The 4B's ES8311 (speaker) and ES7210 (microphones) on one I2S bus at
 * 16 kHz, through esp_codec_dev, as main/speaker_app.c drives the same pair.
 * Recording runs the end-of-speech detector as it goes. The mic hook is
 * called before the mics open and after they close, for the LISTENING banner.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "vad.h"

#define AUDIO_FS 16000

esp_err_t audio_init(void);
int audio_record(int16_t *buf, int max_samples, int max_ms, vad_result_t *why);
bool audio_play_begin(void);
bool audio_play_chunk(const int16_t *mono, int samples);
void audio_play_end(void);
void audio_set_volume(int level);
int audio_volume(void);
void audio_on_mic(void (*cb)(bool open));

#endif /* AUDIO_H */
