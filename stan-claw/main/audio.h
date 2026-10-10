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

/* Called with each 20 ms frame's loudness (RMS) while recording, for the
   listening page's waveform. From the recording task: keep it short. */
void audio_on_level(void (*cb)(int rms));

/* Ends a recording in progress at the next frame (a tap ending a conversation);
   audio_record then reports VAD_SILENT. Cleared when the next recording starts. */
void audio_abort(void);

/* Developer tools for the console. audio_slot_levels records `ms` of the
   ES7210's raw TDM frames (all four slots), playing `tone` (looped, may be
   NULL) as it goes, and gives each slot's peak, RMS, and the RMS of its
   loudest 20 ms (what the end-of-speech detector compares); it returns the
   frames read, -1 with no mics. audio_mic_reg reads an ES7210 register,
   -1 if the read fails. */
int audio_slot_levels(int ms, const int16_t *tone, int tone_len, int peak[4], int rms[4], int loud[4]);
int audio_mic_reg(int reg);

#endif /* AUDIO_H */
