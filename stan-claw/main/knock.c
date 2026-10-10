#include "knock.h"

#include <math.h>
#include <string.h>

void knock_init(knock_t *k)
{
    memset(k, 0, sizeof *k);
    k->first_at = KNOCK_NONE;
}

bool knock_feed(knock_t *k, float ax, float ay, float az, long t_ms)
{
    float dx = ax - k->ax, dy = ay - k->ay, dz = az - k->az;
    k->ax = ax; k->ay = ay; k->az = az;
    if (!k->primed) { k->primed = true; return false; }
    k->last_jolt = sqrtf(dx * dx + dy * dy + dz * dz);
    if (t_ms < k->hush_until) return false;
    if (t_ms < k->quiet_until || k->last_jolt < KNOCK_G) return false;
    k->quiet_until = t_ms + KNOCK_QUIET_MS;
    return true;
}

void knock_hush(knock_t *k, long until_ms)
{
    k->hush_until = until_ms;
    k->first_at = KNOCK_NONE;        /* the touch that caused this is not half of a double knock */
}

bool knock_double(knock_t *k, long t_ms)
{
    if (k->first_at != KNOCK_NONE) {
        long dt = t_ms - k->first_at;
        if (dt >= KNOCK2_MIN_MS && dt <= KNOCK2_MAX_MS) {
            k->first_at = KNOCK_NONE;         /* the pair is consumed */
            return true;
        }
    }
    k->first_at = t_ms;                       /* too soon, too late, or none pending: the new first */
    return false;
}
