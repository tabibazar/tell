#ifndef TTS_H
#define TTS_H
/* Text to ElevenLabs, the speech played as it streams in. 200 ok; 0 key
   missing; TTS_NO_VOICE voice missing; otherwise the HTTP status, -1 no
   connection. */
#define TTS_NO_VOICE (-2)
int tts_speak(const char *text);

/* The same in a given voice -- for the settings page's samples. */
int tts_speak_in(const char *voice_id, const char *text);
#endif /* TTS_H */
