#include "rssi.h"

#include <stdio.h>

static int failures;
static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

int main(void)
{
    /* The anchor the model is built on: the reference level is one metre. */
    float at_ref = rssi_distance_m((int)RSSI_AT_1M);
    expect("the reference level is about a metre", at_ref > 0.9f && at_ref < 1.1f);

    /* Quieter must always mean further. A sign slip would be invisible in any
       single reading and obvious here. */
    {
        float last = 0.0f;
        int ok = 1;
        for (int r = -20; r >= -95; r--) {
            float d = rssi_distance_m(r);
            if (d < last) ok = 0;
            last = d;
        }
        expect("quieter is always further", ok);
    }

    expect("a strong signal is a few metres", rssi_distance_m(-55) < 5.0f);
    expect("a weak one is tens", rssi_distance_m(-85) > 15.0f);

    /* "No reading" must not read as "in your hand". */
    expect("zero means no reading, not zero metres", rssi_distance_m(0) == 0.0f);

    /* Absurd inputs are clamped rather than producing absurd distances. */
    expect("an impossibly loud reading is clamped", rssi_distance_m(50) >= 0.5f);
    expect("and an impossibly quiet one", rssi_distance_m(-200) <= 300.0f);

    expect("four bars at the top", rssi_bars(-40) == 4);
    expect("none at the bottom", rssi_bars(-95) == 0);
    expect("none for no reading", rssi_bars(0) == 0);
    {
        int ok = 1, last = 4;
        for (int r = -30; r >= -100; r--) {
            int b = rssi_bars(r);
            if (b > last) ok = 0;
            last = b;
        }
        expect("bars never rise as the signal falls", ok);
    }

    printf("%s\n", failures ? "FAILURES" : "all tests passed");
    return failures ? 1 : 0;
}
