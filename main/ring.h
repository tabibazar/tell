#ifndef RING_H
#define RING_H

/*
 * speaker's light ring, as seven RGB triples: how loud the room is right now
 * (how far round the ring is lit) and how loud it has been for the last three
 * seconds (the colour). One call a frame at 50 Hz from the LED task; the
 * driver sends `out` down the chain as it is. Pure: no clock, no hardware, no
 * allocation -- the caller brings the levels, the flags and the frame time.
 * See docs/superpowers/specs/2026-09-25-speaker-noise-monitor-design.md.
 *
 * - Fill: LAF from 35 to 85 dBA across the seven LEDs, the last lit one
 *   partly lit. Below 35 dBA the first LED still glows dimly -- the
 *   heartbeat that says the monitor is alive.
 * - Colour: LAeq,3s through green (0,200,60) at 45 dBA and below, yellow
 *   (255,200,0) at 55, orange (255,110,0) at 65 and red (255,0,0) at 75 and
 *   above, mixed in linear light.
 * - Glide: fill and colour approach their targets exponentially, so an 8 Hz
 *   LAF does not step and nothing overshoots. The fill rises with a 40 ms
 *   time constant, so one loud 125 ms block (a clap) shows at 95 % of its
 *   level, and falls with 150 ms, easing back. The colour uses 150 ms both
 *   ways.
 * - Brightness: a ceiling of 25 % of the LED's full drive by day and 8 % by
 *   night; off is all zero. Below the ceiling, partial LEDs and the heartbeat
 *   are gamma-corrected so half an LED looks half as bright.
 * - Not measuring (no audio yet, or gated while the chime plays): the fill
 *   falls to the heartbeat and the colour holds where it was.
 *
 * The ring.c comments carry the reasoning behind each of these.
 */
#include <stdbool.h>
#include <stdint.h>

#define RING_LEDS       7

#define RING_DAY_CAP    0.25f   /* the defaults for ring_cfg_t's ceilings */
#define RING_NIGHT_CAP  0.08f

typedef struct { uint8_t r, g, b; } ring_rgb_t;

typedef struct {
    /* Ceilings on the drive, 0..1 of full (0.25 is 64 of 255). Out of range
       is clamped; 0 is dark, heartbeat and all. */
    float day_cap, night_cap;
    /* Where the fill starts, since where chain index 0 sits on the ring is
       not known until someone looks: `first` is the chain index of the
       quietest LED (the heartbeat's), taken modulo 7, and the fill runs up
       the chain from it, or down it when `reverse`. */
    uint8_t first;
    bool reverse;
} ring_cfg_t;

typedef struct {
    float laf_dba;     /* the fast level, 125 ms: the fill */
    float laeq3_dba;   /* LAeq,3s: the colour */
    float dt_s;        /* seconds since the last frame (0.02 at 50 Hz) */
    bool valid;        /* false: no audio yet, or gated */
    bool night;        /* the night window: the night ceiling */
    bool enabled;      /* false (`!ring off`): all zero */
} ring_in_t;

typedef struct {
    ring_cfg_t cfg;    /* set by ring_init; the app may change it any time */
    float fill;        /* glided, in LEDs, 0..RING_LEDS */
    float colour_dba;  /* glided level the colour is read at, 45..75 */
} ring_t;

/* Defaults, and a ring that is quiet and green: nothing filled yet. */
void ring_init(ring_t *r);

/* One frame: glides toward the inputs by in->dt_s and writes the seven
   triples, index 0..6 in chain order. */
void ring_frame(ring_t *r, const ring_in_t *in, ring_rgb_t out[RING_LEDS]);

/* The two targets the glide heads for, exposed for the tests. */
float ring_fill_for(float laf_dba);                 /* LEDs lit, 0..RING_LEDS; NaN is 0 */
void ring_colour_for(float laeq_dba, float lin[3]); /* linear light, 0..1 per channel */

#endif
