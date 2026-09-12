#ifndef RSSI_H
#define RSSI_H

/*
 * Guessing how far away a transmitter is from how loud it sounds.
 *
 * The log-distance path loss model: signal falls off with the logarithm of
 * distance, so
 *
 *     d = 10 ^ ((P1m - rssi) / (10 * n))
 *
 * where P1m is what it would measure at one metre and n is how fast the
 * signal is eaten by the building.
 *
 * Be honest about what this is worth. n is 2 in open air and somewhere
 * between 2.7 and 4 indoors depending on what the walls are made of; a
 * plasterboard partition costs a few dB and a brick wall ten or more. The
 * transmitter's own power varies between access points, and the reading
 * swings several dB just from turning the board round. So this is good for
 * "nearer or further" and for comparing two readings taken the same way, and
 * it is not good for "four metres". It is reported to one significant figure
 * for that reason: a second digit would be a lie about the precision.
 */

/* What a typical consumer access point measures at one metre. */
#define RSSI_AT_1M (-45.0f)

/* Indoors, through ordinary domestic walls. 2.0 would be open air. */
#define RSSI_PATH_LOSS 2.7f

/* Metres, estimated. Clamped to something sane at both ends: a reading of 0
   means "no reading" rather than "touching it", and nothing usable is
   further than a few hundred metres. */
float rssi_distance_m(int rssi);

/* Four bars' worth of "is this any good", which is what most people actually
   want from a number like -67. 0 is unusable, 4 is excellent. */
int rssi_bars(int rssi);

#endif /* RSSI_H */
