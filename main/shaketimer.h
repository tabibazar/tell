#ifndef SHAKETIMER_H
#define SHAKETIMER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * A countdown you set going by shaking the board, and nothing else.
 *
 * No buttons, no posture. The hourglass this replaces wanted the board stood
 * on its edge and turned over, which is a lot to ask of a thing that lives
 * flat on a desk; a shake works lying down, in one hand, at any angle.
 *
 * Two small pieces, both free of sensors and panels so they run on the host:
 * a detector that turns accelerometer readings into "that was a shake", and
 * a clock that counts down.
 */

/* ---- the detector ---------------------------------------------------- */

typedef struct {
    float fx, fy, fz;      /* slow view of where gravity is */
    bool  primed;          /* the filter has seen a first reading */
    int64_t quiet_until_us;
} shakedet_t;

/* Shakes closer together than this are one shake. A deliberate rattle lasts
   the best part of a second, and without this it would report a dozen. */
#define SHAKE_REFRACTORY_US (900 * 1000)

void shakedet_init(shakedet_t *d);

/*
 * Feeds one accelerometer reading in g. Returns true the moment a shake is
 * recognised.
 *
 * A shake is not a direction, it is energy, so what counts is how far the
 * reading is from the slow view of gravity rather than where gravity points.
 * That makes it work in any orientation, which is the whole point.
 */
bool shakedet_update(shakedet_t *d, float ax, float ay, float az,
                     float dt, int64_t now_us);

/* ---- the clock ------------------------------------------------------- */

typedef enum {
    ST_IDLE = 0,   /* never shaken; showing the full time */
    ST_RUNNING,
    ST_DONE,
    ST_SETTING,    /* being dialled to a new duration */
} st_state_t;

typedef struct {
    int   duration_s;
    float elapsed_s;
    float set_s;       /* the duration being dialled, kept fractional so a
                          slow turn accumulates instead of rounding away */
    st_state_t state;
} shaketimer_t;

/* A minute is a long way to dial and a day is further than anyone wants. */
#define ST_MIN_S 5
#define ST_MAX_S (99 * 60)

void shaketimer_init(shaketimer_t *t, int duration_s);

/* A shake always means the same thing: start again from the top. It is the
   only control there is, so it cannot be ambiguous. */
void shaketimer_shake(shaketimer_t *t);

void shaketimer_tick(shaketimer_t *t, float dt);

/*
 * Setting the duration on the board itself, with no Mac in it.
 *
 * Two rates rather than two positions, because a dial you turn is forgiving
 * and a dial you point is not: let go and it stays where it is, instead of
 * snapping to whatever angle your hand happens to be at.
 *
 * `minutes_rate` and `seconds_rate` are per second of real time, scaled by
 * dt. The caller decides what gesture produces them.
 */
void shaketimer_begin_set(shaketimer_t *t);
bool shaketimer_setting(const shaketimer_t *t);
void shaketimer_adjust(shaketimer_t *t, float minutes_rate, float seconds_rate,
                       float dt);
void shaketimer_accept(shaketimer_t *t);

/*
 * A shuttle curve: turns how far something is held from rest into how fast
 * the value should move, signed, so the same gesture goes both ways.
 *
 * Squared rather than proportional. Held a little it crawls, which is what
 * you want when you are a few seconds out; held hard it runs, which is what
 * you want when you are twenty minutes out. A straight line cannot do both
 * -- it is either too slow to cross the range or too fast to land on a
 * number.
 *
 * Below `deadzone` it is exactly zero: a hand holding a board is never quite
 * still, and a dial that creeps while you read it is useless.
 */
float shaketimer_shuttle(float held, float deadzone, float max_rate);

float shaketimer_remaining_s(const shaketimer_t *t);
st_state_t shaketimer_state(const shaketimer_t *t);

#endif /* SHAKETIMER_H */
