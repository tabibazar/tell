#ifndef FX_H
#define FX_H

/*
 * The robots' voices, made on the board from a human one as it streams in:
 *   FX_HAL     a short resonant comb (a faint metal room), a little dulled
 *   FX_JARVIS  a brighter comb plus a 90 Hz ring modulation mixed in low:
 *              the synthetic shimmer
 * Applied in place to 16 kHz mono PCM, a chunk at a time; never clips.
 * Pure: host_tests/test_sc_voices.c.
 */
#include <stdint.h>

typedef enum { FX_NONE, FX_HAL, FX_JARVIS } fx_kind_t;

#define FX_DELAY_MAX 128

typedef struct {
    fx_kind_t kind;
    float line[FX_DELAY_MAX];
    int at;
    float lp;                 /* the dulling low-pass's state */
    float phase;              /* the ring modulator's */
} fx_t;

void fx_init(fx_t *f, fx_kind_t kind);
void fx_run(fx_t *f, int16_t *pcm, int n);
fx_kind_t fx_from_name(const char *name);   /* "hal", "jarvis", anything else FX_NONE */
const char *fx_name(fx_kind_t kind);        /* "hal", "jarvis", "" */

#endif /* FX_H */
