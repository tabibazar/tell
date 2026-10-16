#ifndef PHRASES_H
#define PHRASES_H

/*
 * A few phrases recorded ahead, in the voice in use, so stan-claw can still
 * speak when the network is slow or gone: "one moment" while Claude looks
 * something up, and plain-words trouble ("I can't reach the internet right
 * now..."). Recorded from ElevenLabs while online (one at a time, by the care
 * task), kept in /mem as raw 16 kHz PCM, recorded again when the voice, the
 * speed or the language (i18n.h) changes; played with the voice's effect and
 * clarity like any speech.
 */
#include <stdbool.h>

typedef enum { PH_MOMENT1, PH_MOMENT2, PH_MOMENT3, PH_OFFLINE, PH_TROUBLE, PH_MISSED, PH_N } phrase_t;

void phrases_load(void);              /* from /mem, at boot (after memory_init) */
bool phrases_stale(void);             /* one is missing, or the voice, speed or language changed */
bool phrases_refresh_one(void);       /* network: records the next missing one; false on failure */
bool phrase_play(phrase_t p);         /* false if it isn't recorded yet (say nothing then) */
const char *phrase_text(phrase_t p);  /* its words in the language in use, for the screen */

#endif /* PHRASES_H */
