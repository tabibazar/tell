#ifndef VOICES_H
#define VOICES_H

/* The ElevenLabs voices on the account (GET /v1/voices), cut down to what the
   settings page shows. "Bella - Professional, Bright" becomes the name Bella
   and the description Professional, Bright; the labels become tags. Pure:
   host_tests/test_sc_voices.c. */
#define VOICES_MAX 40

typedef struct {
    char id[32];
    char name[24];
    char desc[48];
    char tags[48];       /* "female, british, middle aged" */
} voice_t;

/* Returns how many were read into out[max], or -1 when it is not a voices reply. */
int voices_parse(const char *json, voice_t *out, int max);

#endif /* VOICES_H */
