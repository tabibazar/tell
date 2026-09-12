#include "shaketimer.h"

/*
 * How fast the slow view of gravity follows the reading. Around a 250 ms
 * time constant at 20 fps: quick enough to keep up with the board being
 * picked up, tilted or set down, and far too slow to follow a shake.
 */
#define FOLLOW 0.20f

/*
 * How far the reading must be from that slow view, in g, before it counts.
 *
 * Gravity alone is 1 g, so a threshold below that could be reached by
 * tilting; well above it can only come from the board being moved sharply.
 * 1.4 takes a deliberate rattle and ignores a firm desk tap.
 */
#define SHAKE_G 1.4f

void shakedet_init(shakedet_t *d)
{
    d->fx = d->fy = d->fz = 0.0f;
    d->primed = false;
    d->quiet_until_us = 0;
}

bool shakedet_update(shakedet_t *d, float ax, float ay, float az,
                     float dt, int64_t now_us)
{
    (void)dt;

    if (!d->primed) {
        /* Start the filter where the board actually is. Letting it converge
           from zero would read as one enormous shake at boot, and the timer
           would start itself every time the board was switched on. */
        d->fx = ax; d->fy = ay; d->fz = az;
        d->primed = true;
        return false;
    }

    float rx = ax - d->fx, ry = ay - d->fy, rz = az - d->fz;
    d->fx += rx * FOLLOW;
    d->fy += ry * FOLLOW;
    d->fz += rz * FOLLOW;

    /* Square of the residual, to keep a square root off the Xtensa, which
       does them in software. */
    float r2 = rx * rx + ry * ry + rz * rz;
    if (r2 < SHAKE_G * SHAKE_G) return false;
    if (now_us < d->quiet_until_us) return false;

    d->quiet_until_us = now_us + SHAKE_REFRACTORY_US;
    return true;
}

void shaketimer_init(shaketimer_t *t, int duration_s)
{
    t->duration_s = duration_s < 0 ? 0 : duration_s;
    t->elapsed_s = 0.0f;
    t->set_s = (float)t->duration_s;
    t->state = ST_IDLE;
}

void shaketimer_begin_set(shaketimer_t *t)
{
    t->set_s = (float)t->duration_s;
    t->state = ST_SETTING;
}

bool shaketimer_setting(const shaketimer_t *t)
{
    return t->state == ST_SETTING;
}

void shaketimer_adjust(shaketimer_t *t, float minutes_rate, float seconds_rate,
                       float dt)
{
    if (t->state != ST_SETTING || dt <= 0.0f) return;
    t->set_s += (minutes_rate * 60.0f + seconds_rate) * dt;
    if (t->set_s < (float)ST_MIN_S) t->set_s = (float)ST_MIN_S;
    if (t->set_s > (float)ST_MAX_S) t->set_s = (float)ST_MAX_S;
}

float shaketimer_shuttle(float held, float deadzone, float max_rate)
{
    float sign = held < 0.0f ? -1.0f : 1.0f;
    float mag = held < 0.0f ? -held : held;
    if (mag <= deadzone) return 0.0f;

    /* Re-spread what is left of the range over 0..1, so the curve starts at
       zero where the dead zone ends rather than jumping. */
    float span = 1.0f - deadzone;
    float t = span > 0.0f ? (mag - deadzone) / span : 1.0f;
    if (t > 1.0f) t = 1.0f;
    return sign * max_rate * t * t;
}

void shaketimer_accept(shaketimer_t *t)
{
    if (t->state != ST_SETTING) return;
    t->duration_s = (int)(t->set_s + 0.5f);
    t->elapsed_s = 0.0f;
    /* Set but not started: a shake is the only thing that starts it, and
       keeping that true means there is never a second way. */
    t->state = ST_IDLE;
}

void shaketimer_shake(shaketimer_t *t)
{
    /* A shake while dialling is a hand being unsteady, not an instruction. */
    if (t->state == ST_SETTING) return;
    t->elapsed_s = 0.0f;
    t->state = ST_RUNNING;
    /* A zero duration has nothing to count, and must not sit running for
       ever waiting for a tick that finishes it. */
    if (t->duration_s <= 0) t->state = ST_DONE;
}

void shaketimer_tick(shaketimer_t *t, float dt)
{
    if (t->state != ST_RUNNING) return;
    if (dt > 0.0f) t->elapsed_s += dt;
    if (t->elapsed_s >= (float)t->duration_s) {
        t->elapsed_s = (float)t->duration_s;
        t->state = ST_DONE;
    }
}

float shaketimer_remaining_s(const shaketimer_t *t)
{
    /* While dialling, what is showing is what is being dialled. */
    if (t->state == ST_SETTING) return t->set_s;
    if (t->state == ST_DONE) return 0.0f;
    float left = (float)t->duration_s - t->elapsed_s;
    return left < 0.0f ? 0.0f : left;
}

st_state_t shaketimer_state(const shaketimer_t *t) { return t->state; }
