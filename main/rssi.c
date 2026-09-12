#include "rssi.h"

#include <math.h>

float rssi_distance_m(int rssi)
{
    /* 0 is what the driver reports for "no reading", and treating it as a
       very loud signal would put a phantom access point in your lap. */
    if (rssi == 0) return 0.0f;
    if (rssi > -20) rssi = -20;          /* closer than this is not resolvable */
    if (rssi < -100) rssi = -100;

    float d = powf(10.0f, (RSSI_AT_1M - (float)rssi) / (10.0f * RSSI_PATH_LOSS));
    if (d < 0.5f) d = 0.5f;
    if (d > 300.0f) d = 300.0f;
    return d;
}

int rssi_bars(int rssi)
{
    /*
     * The thresholds people actually use: above -55 is as good as it gets,
     * below -80 is where things start failing rather than merely slowing.
     */
    if (rssi == 0) return 0;
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}
