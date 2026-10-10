#ifndef SPEECHFMT_H
#define SPEECHFMT_H

/* What goes to Deepgram and ElevenLabs and what comes back. Pure:
   host_tests/test_sc_speech.c. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void wav_header(uint8_t out[44], int sample_rate, int samples);
bool deepgram_transcript(const char *json, char *out, size_t n);
char *elevenlabs_body(const char *text);

#endif /* SPEECHFMT_H */
