#ifndef BOARDSTATS_H
#define BOARDSTATS_H

/*
 * A board's numbers to the boards' dashboard (stan-mcp/boards.py), over WiFi.
 *
 * Each report is one HTTPS POST of JSON: the board's name and MAC, a "sys"
 * section every board gets for free (uptime, heap, PSRAM, chip temperature,
 * the network and its signal, why it last reset) and a "stats" section the
 * board fills from its fill callback with boardstats_num / boardstats_str.
 * A key's last part names its unit for the page: temp_c, humidity_pct,
 * pressure_hpa, laf_dba, eco2_ppm, tvoc_ppb, ... (see boards.py).
 *
 * The networks, the address and the token come from the gitignored secrets/
 * at build time (CMakeLists.txt); with no BOARDS_URL nothing is sent.
 *
 * Two ways to use it:
 *   boardstats_start()   a task that stays on the network and reports every
 *                        `every_s` seconds (speaker: mains-powered, radio free)
 *   boardstats_report()  one report now, from the caller's task, connecting
 *                        first and (keep == false) letting go of the radio
 *                        after (tiny1: the radio belongs to ESP-NOW otherwise)
 */
#include <stdbool.h>

typedef void (*boardstats_fill_t)(void);

void boardstats_num(const char *key, double v);      /* NaN is left out */
void boardstats_str(const char *key, const char *v);

/* WiFi must already be initialised (esp_wifi_init) by the board, or not at
   all: boardstats does it when it has not been. Returns true when the
   dashboard took the report. */
bool boardstats_report(const char *board, int every_s, boardstats_fill_t fill, bool keep, int timeout_ms);

void boardstats_start(const char *board, int every_s, boardstats_fill_t fill);

#endif /* BOARDSTATS_H */
