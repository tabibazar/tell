#ifndef VOICES_H
#define VOICES_H

/* stan-claw's ten voices: four men, four women and two machines. The people
   are ElevenLabs' stock voices (their ids are public); the machines are a
   stock voice through an effect made on the board (fx.h). Pure:
   host_tests/test_sc_voices.c. */
#include "fx.h"

#define VOICES_N 10

typedef struct {
    const char *id;          /* the ElevenLabs voice it speaks with */
    const char *name;
    const char *desc;
    const char *tags;
    fx_kind_t fx;
} voice_t;

extern const voice_t VOICES[VOICES_N];

/* The entry for a saved choice (voice id + effect name), or -1. */
int voices_find(const char *id, const char *fx_name);

#endif /* VOICES_H */
