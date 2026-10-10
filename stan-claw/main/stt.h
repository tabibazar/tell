#ifndef STT_H
#define STT_H
#include <stddef.h>
#include <stdint.h>
/* A recording to Deepgram, the transcript back. 200 ok ("" = heard
   nothing); 0 key missing; otherwise the HTTP status, -1 no connection. */
int stt_transcribe(const int16_t *pcm, int samples, char *out, size_t n);
#endif /* STT_H */
