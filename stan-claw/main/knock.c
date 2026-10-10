#include "knock.h"

#include <math.h>
#include <string.h>

void knock_init(knock_t *k) { memset(k, 0, sizeof *k); }

bool knock_feed(knock_t *k, float ax, float ay, float az, long t_ms)
{
    float dx = ax - k->ax, dy = ay - k->ay, dz = az - k->az;
    k->ax = ax; k->ay = ay; k->az = az;
    if (!k->primed) { k->primed = true; return false; }
    k->last_jolt = sqrtf(dx * dx + dy * dy + dz * dz);
    if (t_ms < k->quiet_until || k->last_jolt < KNOCK_G) return false;
    k->quiet_until = t_ms + KNOCK_QUIET_MS;
    return true;
}
