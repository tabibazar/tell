/* stan-claw's rest: when the screen breathes and sleeps, how the breath
   moves, and knocks on the desk out of accelerometer readings. */
#include "knock.h"
#include "rest.h"
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    int noon = 12 * 60, eve = 19 * 60, morning = 8 * 60;
    CHECK(rest_level(0, noon) == REST_AWAKE);
    CHECK(rest_level(44, noon) == REST_AWAKE);
    CHECK(rest_level(45, noon) == REST_BREATHING);
    CHECK(rest_level(45 + 299, noon) == REST_BREATHING);
    CHECK(rest_level(45 + 300, noon) == REST_ASLEEP);
    CHECK(rest_level(45, eve) == REST_ASLEEP);              /* 19:00: straight to dark */
    CHECK(rest_level(45, eve - 1) == REST_BREATHING);       /* 18:59 */
    CHECK(rest_level(45, morning - 1) == REST_ASLEEP);      /* 07:59 */
    CHECK(rest_level(45, morning) == REST_BREATHING);       /* 08:00 */
    CHECK(rest_level(10, 23 * 60) == REST_AWAKE);           /* used at night: awake */
    CHECK(rest_level(45, -1) == REST_BREATHING);            /* no clock yet: no night */

    CHECK(rest_breath_light(0) == 3);
    CHECK(rest_breath_light(REST_BREATH_MS / 2) == 14);
    CHECK(rest_breath_light(REST_BREATH_MS) == 3);
    int lo = 100, hi = 0;
    for (long t = 0; t < REST_BREATH_MS; t += 50) {
        int v = rest_breath_light(t);
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
    }
    CHECK(lo == 3 && hi == 14);

    /* Office hours: Monday to Friday, 08:00-16:00. */
    CHECK(rest_office(1, 8 * 60) && rest_office(5, 16 * 60 - 1));
    CHECK(!rest_office(1, 8 * 60 - 1) && !rest_office(3, 16 * 60) && !rest_office(0, 10 * 60) && !rest_office(6, 10 * 60));
    CHECK(rest_until_office(2, 10 * 60, 0) == 0);                          /* Tuesday 10:00: in */
    CHECK(rest_until_office(2, 7 * 60 + 59, 30) == 30);                    /* Tuesday 07:59:30 */
    CHECK(rest_until_office(2, 16 * 60, 0) == 16 * 3600);                  /* Tuesday 16:00 -> Wednesday 08:00 */
    CHECK(rest_until_office(5, 16 * 60, 0) == 2 * 86400 + 16 * 3600);      /* Friday 16:00 -> Monday 08:00 */
    CHECK(rest_until_office(6, 12 * 60, 0) == 86400 + 20 * 3600);          /* Saturday noon -> Monday 08:00 */
    CHECK(rest_until_office(0, 23 * 60 + 59, 59) == 8 * 3600 + 1);         /* Sunday 23:59:59 */

    knock_t k;
    knock_init(&k);
    long t = 0;
    CHECK(!knock_feed(&k, 0.0f, 0.0f, 1.0f, t));             /* first reading primes */
    for (int i = 0; i < 50; i++) { t += 30; CHECK(!knock_feed(&k, 0.004f * (i & 1), 0.0f, 1.0f, t)); }  /* still, sensor noise */
    t += 30;
    CHECK(knock_feed(&k, 0.0f, 0.0f, 1.25f, t));             /* a knock */
    t += 30;
    CHECK(!knock_feed(&k, 0.0f, 0.0f, 0.85f, t));            /* its ring-down, inside the quiet spell */
    for (int i = 1; i <= 25; i++) {                          /* it settles back, read every 30 ms */
        t += 30;
        CHECK(!knock_feed(&k, 0.0f, 0.0f, 0.85f + 0.15f * (i < 5 ? i / 5.0f : 1.0f), t));
    }
    t += 30;
    CHECK(knock_feed(&k, 0.15f, 0.0f, 1.0f, t));             /* a second knock after the quiet spell */
    knock_init(&k);
    knock_feed(&k, 0.0f, 0.7f, 0.7f, 0);                     /* tilted panel: gravity does not count */
    CHECK(!knock_feed(&k, 0.0f, 0.7f, 0.7f, 30));

    printf(fails ? "%d FAILED\n" : "sc_rest: all passed\n", fails);
    return fails != 0;
}
