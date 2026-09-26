#ifndef SPEAKER_APP_H
#define SPEAKER_APP_H

/*
 * speaker: a noise monitor on the Waveshare ESP32-S3-AUDIO-Board. No screen,
 * so none of main.c's page UI: app_main calls this first thing on her board
 * and returns straight after it.
 *
 * It brings the board up in the order the hardware demands (the ring cleared
 * before anything else, the amp held off before anything could turn it on,
 * the codecs clocked before they are spoken to), starts the tasks that
 * measure, light the ring and log, and returns. The tasks carry on without
 * it. See docs/superpowers/specs/2026-09-25-speaker-noise-monitor-design.md,
 * and docs/hardware/speaker-pinout.md for why the order is what it is.
 *
 * On the USB serial port, besides the log, it talks to the Mac's relay
 * (tools/noise-relay.py): a "days: <cal|est> <date>=<LAeq>/<L90> ..." line
 * once a minute and after each !cal, the history the relay hands on to watch;
 * and from the relay, the same "!" commands BLE takes, and "!play <nbytes>
 * 16000 <normal|test>" followed by that much int16 mono PCM, spoken through
 * the chime's amp sequence and answered with "play: ..." lines on the log.
 */
void speaker_app_main(void);

#endif /* SPEAKER_APP_H */
