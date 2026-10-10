#ifndef TTS_H
#define TTS_H
/* Text to ElevenLabs, the speech played as it streams in. 200 ok; 0 key
   missing; TTS_NO_VOICE voice missing; otherwise the HTTP status, -1 no
   connection. */
#define TTS_NO_VOICE (-2)
int tts_speak(const char *text);

#include "fx.h"

/* The same in a given voice and effect -- for the settings page's samples. */
int tts_speak_as(const char *voice_id, fx_kind_t fx, const char *text);
#endif /* TTS_H */
