#ifndef LIVEUI_H
#define LIVEUI_H

/*
 * speaker's Live page: what the microphone hears right now, twenty-odd times
 * a second. 240x320 portrait, top to bottom:
 *
 *   - LIVE, and the fast level in dBA at the right, in the ring's colour;
 *   - the waveform: the last LIVEUI_WAVE samples (30 ms at 16 kHz), two to a
 *     column as a min/max envelope about a faint centre line, scaled to the
 *     loudest of the moment so a quiet room still shows its shape;
 *   - 22 third-octave bands, 63 Hz to 8 kHz, as horizontal bars on a fixed
 *     20..90 dB scale, each in the ring's colour for its own level, with a
 *     peak marker that the caller lets fall back; the octaves labelled.
 *
 * Pure: the spectrum is computed here from samples handed in (a 2048-point
 * FFT, Hann-windowed), and the page drawn from a struct; no clock, no
 * hardware, so both are tested and rendered on the host.
 */
#include "canvas.h"

#include <stdbool.h>
#include <stdint.h>

#define LIVEUI_WIDTH   240
#define LIVEUI_HEIGHT  320
#define LIVEUI_WAVE    480      /* samples in the waveform: 30 ms at 16 kHz */
#define LIVEUI_FFT     2048     /* samples the spectrum is taken over: 128 ms, bins 7.8 Hz apart */
#define LIVEUI_BANDS   22       /* thirds of an octave, centred 1 kHz x 2^(k/3) for k = -12..9:
                                    62.5 Hz to 8 kHz */

typedef struct {
    bool  have_signal;
    float laf;                      /* dBA, the fast level, for the header */
    bool  calibrated;
    int16_t wave[LIVEUI_WAVE];      /* oldest first */
    float band[LIVEUI_BANDS];       /* dB, as liveui_bands gives them; NaN draws nothing */
    float peak[LIVEUI_BANDS];       /* dB, the held peaks; NaN draws none */
} liveui_t;

/*
 * The third-octave bands of LIVEUI_FFT samples at `fs` Hz, in dB: each band's
 * share of the mean square (samples scaled to +-1, Hann window and its power
 * allowed for), as dBFS plus `offset_db` -- speaker's calibration offset, so
 * the bars stand near the level the page's header shows. Unweighted: the
 * low bands read what is there, which A-weighting would hide.
 */
void liveui_bands(const int16_t *x, float fs, float offset_db, float out_db[LIVEUI_BANDS]);

/* The page, onto a canvas of LIVEUI_WIDTH x LIVEUI_HEIGHT (clipped to any
   other size). NULL draws the ground alone. */
void liveui_draw(canvas_t *c, const liveui_t *s);

#endif /* LIVEUI_H */
