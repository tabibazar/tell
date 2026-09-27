#ifndef DIRUI_H
#define DIRUI_H

/*
 * speaker's Direction page: where a sound comes from, by how much sooner it
 * reaches one microphone than the other.
 *
 * The estimate is GCC-PHAT: the cross-spectrum of the two mics, each bin
 * reduced to its phase (so every frequency votes equally, and the room's
 * colour does not), searched over delays in fractions of a sample. It has
 * to be: the capsules are DIRUI_SPACING_M apart, so the longest delay there
 * is -- a sound straight along their axis -- is 34.5 mm / 343 m/s = 101 us,
 * 1.6 samples at 16 kHz. Only 300 Hz to 4.5 kHz is used: below, the room's
 * rumble has no direction worth the name; above c / 2d = 5 kHz the phase
 * wraps and a delay has two answers.
 *
 * Two mics give a bearing to their axis and no more: a sound in front and
 * one behind at the mirror angle arrive with the same delay, so the page is
 * a half circle, -90 (MIC 1's end) through 0 (broadside) to +90 (MIC 2's).
 *
 * Pure: samples in, an angle out, and the page drawn from a struct.
 */
#include "canvas.h"

#include <stdbool.h>
#include <stdint.h>

#define DIRUI_WIDTH     240
#define DIRUI_HEIGHT    320
#define DIRUI_N         512         /* samples per estimate: 32 ms at 16 kHz */
#define DIRUI_SPACING_M 0.0345f     /* MIC1 to MIC2, docs/superpowers/specs speaker design */
#define DIRUI_TRAIL     24          /* recent bearings shown fading on the arc */

/*
 * The bearing of the sound in two blocks of DIRUI_N samples, MIC1's and
 * MIC2's, in degrees: -90 is along the axis at MIC1's end (MIC1 hears it
 * first), +90 at MIC2's. `confidence` (may be NULL) is how clearly one delay
 * stands out, 0..1: the phase-transform's peak, which a single source makes
 * near 1 and diffuse noise near 0. False (and nothing set) for silence.
 */
bool dirui_estimate(const int16_t *mic1, const int16_t *mic2, float fs, float *deg, float *confidence);

typedef struct {
    bool  have_signal;      /* the meter is running */
    bool  active;           /* a sound loud and clear enough to place, just now */
    float deg;              /* the latest bearing, smoothed */
    float confidence;
    float laf;              /* dBA, for the header */
    int   n_trail;
    float trail[DIRUI_TRAIL];       /* recent bearings, oldest first */
} dirui_t;

void dirui_draw(canvas_t *c, const dirui_t *s);

#endif /* DIRUI_H */
