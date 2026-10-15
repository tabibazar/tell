#ifndef CONSOLE_H
#define CONSOLE_H

#include <stddef.h>

/*
 * stan-claw's settings over its USB serial (115200), a line at a time:
 *   wifi add NAME PASSWORD | wifi forget NAME | wifi list | wifi setup (from a phone, by the screen's QR code)
 *   key claude|deepgram|elevenlabs KEY      (never echoed back)
 *   voice NAME (from the wheel) or ID | model ID | mcp url URL | mcp token TOKEN | serve token TOKEN
 *   status
 *   clarity on|off | text large|xl | contrast normal|high
 *   hoa token TOKEN | hoa forget        (the homeowner's stan-mcp token, for reminders and family messages)
 *   care | care check | care test reminder|message|checkin
 *   weather | weather at LAT,LON | weather at auto   (the resting screen's place: by default where the
 *                                      internet connection says home is, Toronto until then)
 * Quote a word with spaces: wifi add "Office WiFi" secret.
 */
void console_start(void);

/* stanclaw.c: re-read the settings the console changed (the voice). */
void settings_reload(void);

/* stanclaw.c: the care task -- fetch reminders and messages now; or put a
   pretend reminder (kind 0) or family message (kind 1) up now, for a demo. */
void care_kick(void);
void care_demo(int kind);

/* stanclaw.c: start WiFi setup from a phone (the QR code on the screen). */
void wifi_setup(void);
void wifi_setup_stop(void);

/* stanclaw.c: where the resting screen's weather is for, and how it was found. */
void weather_where(char *out, size_t n);

#endif /* CONSOLE_H */
