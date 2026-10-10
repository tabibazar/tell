#ifndef TTS_H
#define TTS_H
/* Text to ElevenLabs, the speech played as it streams in. 200 ok; 0 key or
   voice missing; otherwise the HTTP status, -1 no connection. */
int tts_speak(const char *text);
#endif /* TTS_H */
