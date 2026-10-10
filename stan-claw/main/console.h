#ifndef CONSOLE_H
#define CONSOLE_H

/*
 * stan-claw's settings over its USB serial (115200), a line at a time:
 *   wifi add NAME PASSWORD | wifi forget NAME | wifi list
 *   key claude|deepgram|elevenlabs KEY      (never echoed back)
 *   voice NAME (from the wheel) or ID | model ID | mcp url URL | mcp token TOKEN | serve token TOKEN
 *   status
 * Quote a word with spaces: wifi add "Office WiFi" secret.
 */
void console_start(void);

/* stanclaw.c: re-read the settings the console changed (the voice). */
void settings_reload(void);

#endif /* CONSOLE_H */
