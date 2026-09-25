#include "ring.h"

#include <math.h>
#include <stddef.h>

/*
 * All in float: the S3's FPU is single precision, and nothing here needs more
 * than the eight bits a WS2812 channel has. A frame costs about a dozen powf
 * and two expf, at 50 Hz.
 */

/* The fill's scale: nothing lit at 35 dBA, all seven at 85, so each LED is
   50/7 = 7.1 dB of the room. */
#define FILL_FROM_DBA   35.0f
#define FILL_TO_DBA     85.0f

/*
 * The glide's two speeds, as time constants. The fill RISES in 40 ms and
 * FALLS in 150, the way a meter's needle kicks up and eases back.
 *
 * Up fast, because the fill is the room right now and a clap is one or two
 * loud 125 ms blocks of LAF: six or seven frames at 50 Hz. Six frames of a
 * 40 ms rise cover 1 - e^-3 = 95 % of the way, so a clap lights as many LEDs
 * as it measured. A single 150 ms time constant got 55 % of the way in the
 * same six frames, 4.2 of 7 LEDs for a clap at 85 dBA in a 40 dBA room: the
 * ring under-read exactly the events it is there to show.
 *
 * Down slowly, so a level coming down in LAF's 8 Hz steps sweeps rather than
 * steps, and a short event stays on the ring long enough to be seen: after
 * a clap the fill is still half up 100 ms later and gone in about 450.
 *
 * The colour is LAeq,3s, already smooth, and glides at 150 ms both ways.
 */
#define RISE_TAU_S      0.04f
#define TAU_S           0.15f

/*
 * Light and the eye. A WS2812's PWM is close to linear in light: 128 gives
 * half the photons of 255. The eye is not: half the light looks about 73 %
 * as bright, and a quarter looks half. A plain power law of 2.2 stands in
 * for that, used one way only -- from how bright a thing should look (or an
 * sRGB byte, which is already in those terms) to the light that makes it,
 * (x)^2.2. The sRGB curve proper differs from it only in the bottom few
 * percent, well under one count of the eight bits here.
 */
#define GAMMA           2.2f

/*
 * The heartbeat: how bright the first LED looks, at the least, whenever the
 * ring is on -- 30 % of a full LED to the eye, 7 % of its light. It is a
 * floor under the first LED's own fill, not a mode of its own, so a level
 * rising through 35 dBA leaves the heartbeat where it is until the fill
 * passes it, and nothing blinks at the handover.
 */
#define HEARTBEAT       0.3f

/*
 * The colour scale: the spec's four stops, as the sRGB bytes it gives them.
 * The colour glides as a level between the first and the last, so it is
 * always a colour on this scale.
 */
static const struct { float dba; uint8_t rgb[3]; } s_stops[] = {
    { 45.0f, {   0, 200, 60 } },   /* green */
    { 55.0f, { 255, 200,  0 } },   /* yellow */
    { 65.0f, { 255, 110,  0 } },   /* orange */
    { 75.0f, { 255,   0,  0 } },   /* red */
};
#define N_STOPS         (sizeof s_stops / sizeof s_stops[0])
#define COLOUR_FROM_DBA 45.0f
#define COLOUR_TO_DBA   75.0f

/* fmaxf and fminf return the other operand when one is NaN, so NaN lands on
   `lo`; -inf and +inf land on the ends, as they should. A level of -inf is
   real: log10 of a block of digital silence. */
static float clampf(float x, float lo, float hi)
{
    return fminf(fmaxf(x, lo), hi);
}

void ring_init(ring_t *r)
{
    r->cfg.day_cap = RING_DAY_CAP;
    r->cfg.night_cap = RING_NIGHT_CAP;
    r->cfg.first = 0;
    r->cfg.reverse = false;
    r->fill = 0.0f;
    r->colour_dba = COLOUR_FROM_DBA;
}

float ring_fill_for(float laf_dba)
{
    float leds = (laf_dba - FILL_FROM_DBA) * (float)RING_LEDS / (FILL_TO_DBA - FILL_FROM_DBA);
    return clampf(leds, 0.0f, (float)RING_LEDS);
}

static float linear(uint8_t srgb)
{
    return powf((float)srgb / 255.0f, GAMMA);
}

/*
 * The colour is mixed in linear light: the two stops either side are turned
 * into light first, blended in proportion, and sent as light, since the LED
 * turns bytes straight into light. Two reasons.
 *
 * Mixing light is what a blend physically is -- the colour two LEDs of those
 * stops would make together -- so a midpoint is as bright as its ends and
 * does not sag. Blending the sRGB bytes instead sags: halfway from yellow to
 * orange it gives green 155, which as light is 0.335 where the linear mean
 * of the two is 0.372, a dimmer and muddier colour in the middle of the
 * band.
 *
 * And the stops are screen colours. Sent to the LED as they are, orange
 * (255,110,0) is 43 % green light and looks yellow; turned into light first,
 * it is (255,40,0), which looks orange on a WS2812 too.
 *
 * Every channel moves one way along the scale (red up, green and blue down),
 * so a colour that glides along it never swings a channel past its target.
 */
void ring_colour_for(float laeq_dba, float lin[3])
{
    float d = clampf(laeq_dba, COLOUR_FROM_DBA, COLOUR_TO_DBA);
    size_t i = 0;
    while (i + 2 < N_STOPS && d > s_stops[i + 1].dba) i++;
    float t = (d - s_stops[i].dba) / (s_stops[i + 1].dba - s_stops[i].dba);
    for (int c = 0; c < 3; c++) {
        /* (1-t)a + tb rather than a + t(b-a): exact at both stops. */
        lin[c] = (1.0f - t) * linear(s_stops[i].rgb[c]) + t * linear(s_stops[i + 1].rgb[c]);
    }
}

/*
 * One step of an exponential approach: `keep` is the part of the gap left
 * after the step, exp(-dt/tau). Written from the target's side, so a keep of
 * zero (a frame many time constants long) lands exactly on the target; and
 * held between where it was and where it is going, so no rounding can carry
 * it past.
 */
static float glide(float from, float to, float keep)
{
    float x = to - (to - from) * keep;
    return from <= to ? clampf(x, from, to) : clampf(x, to, from);
}

/* Where logical LED i (0 the quietest) sits in the chain. */
static int chain_index(const ring_cfg_t *cfg, int i)
{
    int first = cfg->first % RING_LEDS;
    return cfg->reverse ? (first - i + RING_LEDS) % RING_LEDS : (first + i) % RING_LEDS;
}

/*
 * Light to bytes. The light is clamped to 0..1 before the cast, since a
 * float past 255 cast to uint8_t is undefined, not 255. `keep_lit` is the
 * heartbeat's guarantee: the first LED is never rounded to black while the
 * ring is on. At night its green works out to 0.85 of a count, and one count
 * on the brightest channel is the dimmest thing a WS2812 can show.
 */
static ring_rgb_t to_bytes(const float lin[3], float light, bool keep_lit)
{
    float v[3];
    uint8_t b[3];
    int brightest = 0;
    for (int c = 0; c < 3; c++) {
        v[c] = clampf(lin[c] * light, 0.0f, 1.0f) * 255.0f;
        b[c] = (uint8_t)(v[c] + 0.5f);
        if (v[c] > v[brightest]) brightest = c;
    }
    if (keep_lit && !b[0] && !b[1] && !b[2] && v[brightest] > 0.0f) b[brightest] = 1;
    return (ring_rgb_t){ b[0], b[1], b[2] };
}

void ring_frame(ring_t *r, const ring_in_t *in, ring_rgb_t out[RING_LEDS])
{
    /*
     * Not measuring -- no audio yet, the gate while the chime plays, or a NaN
     * from upstream -- empties the fill down to the heartbeat and holds the
     * colour. Held, because a red room gated for a chime should not flash
     * green for the second the chime takes; a ring with no audio yet holds
     * the green it started in.
     */
    bool measuring = in->valid && !isnan(in->laf_dba) && !isnan(in->laeq3_dba);
    float fill_to = measuring ? ring_fill_for(in->laf_dba) : 0.0f;
    /* The colour heads for the scale's end, not the raw level: a room at
       95 dBA that falls to 60 leaves red at once rather than first gliding
       down through twenty dB that all look the same red. */
    float colour_to = measuring
        ? clampf(in->laeq3_dba, COLOUR_FROM_DBA, COLOUR_TO_DBA) : r->colour_dba;

    /* Both glide whether the ring is on or not, so `!ring on` lands on the
       room as it is. A frame of no time, negative time or NaN moves nothing
       (the test is written so NaN fails it); an infinite one lands. */
    if (in->dt_s > 0.0f) {
        float fill_tau = fill_to > r->fill ? RISE_TAU_S : TAU_S;
        r->fill = glide(r->fill, fill_to, expf(-in->dt_s / fill_tau));
        r->colour_dba = glide(r->colour_dba, colour_to, expf(-in->dt_s / TAU_S));
    }

    for (int i = 0; i < RING_LEDS; i++) out[i] = (ring_rgb_t){ 0, 0, 0 };

    /*
     * The ceiling is on the light itself -- the LED's duty, which is also the
     * current it draws from the shared 3V3 buck -- not on how bright it looks.
     * Read as looks, 8 % would be 0.08^2.2 = 0.4 % of the light, one count of
     * 255: night orange and night red would both be (1,0,0), night green
     * (0,1,0), and a partial LED would have no steps at all, on a ring that
     * is meant to keep measuring through the night. As light, the ceiling
     * leaves 20 counts by night (orange (20,3,0), red (20,0,0)) and 64 by
     * day. What does go through the gamma is everything under the ceiling:
     * how much of an LED is lit, and the heartbeat.
     */
    float cap = clampf(in->night ? r->cfg.night_cap : r->cfg.day_cap, 0.0f, 1.0f);
    if (!in->enabled || !(cap > 0.0f)) return;

    float lin[3];
    ring_colour_for(r->colour_dba, lin);

    /*
     * How much of each LED is lit is how bright it should look, and runs
     * linearly in dB across the LED's 7.1 dB: halfway through looks half as
     * bright, 0.5^2.2 = 22 % of the light. Sent as a plain half it would look
     * nearly full and the sweep would jump at each LED's start.
     */
    for (int i = 0; i < RING_LEDS; i++) {
        float amount = clampf(r->fill - (float)i, 0.0f, 1.0f);
        if (i == 0) amount = fmaxf(amount, HEARTBEAT);
        if (amount <= 0.0f) continue;
        out[chain_index(&r->cfg, i)] = to_bytes(lin, cap * powf(amount, GAMMA), i == 0);
    }
}
