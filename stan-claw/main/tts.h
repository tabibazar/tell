#ifndef TTS_H
#define TTS_H
/* Text to ElevenLabs, the speech played as it streams in. 200 ok; 0 key
   missing; TTS_NO_VOICE voice missing; otherwise the HTTP status, -1 no
   connection. */
#define TTS_NO_VOICE (-2)
int tts_speak(const char *text);

/* Stops speech in progress (a tap ending a conversation). */
void tts_abort(void);

/* The same at a given speed (0.7-1.2; anything else: the setting) -- "Slower please". */
int tts_speak_slow(const char *text, float speed);

/* The speaking speed set (0.85, 1.0, 1.15). */
float tts_speed(void);

/* Samples played so far of the speech in progress (16 kHz), for the captions. */
int tts_played(void);

#include "fx.h"

/* The same in a given voice and effect -- for the settings page's samples. */
int tts_speak_as(const char *voice_id, fx_kind_t fx, const char *text);
#endif /* TTS_H */
