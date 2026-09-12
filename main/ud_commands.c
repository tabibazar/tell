#include "ud_internal.h"

#include <stddef.h>

/*
 * The IMU pages -- the Feather's spirit level and lilly's liquid -- are
 * reached and adjusted by marker, like everything else, but these markers
 * carry no data: they are commands. They
 * are sections anyway so that every marker the board understands lives in one
 * registry rather than as special cases scattered through the parser, and
 * main acts on the kind that comes back.
 *
 * Every board compiles this. A board with no IMU never offers the pages, so
 * the markers simply do nothing there.
 */
static void no_payload(usagedata_t *d, ud_host_t *h,
                       const char *tag, const char *rest)
{
    (void)d; (void)h; (void)tag; (void)rest;
}

#define COMMAND(name, marker, kind) \
    const ud_section_t ud_section_##name = { \
        marker, kind, false, NULL, no_payload, NULL, NULL, NULL }

COMMAND(particles, "!particles", UD_PARTICLES);
COMMAND(timer,     "!timer",     UD_TIMER);
COMMAND(page,      "!page",      UD_PAGE);
COMMAND(stopwatch, "!stopwatch", UD_STOPWATCH);
COMMAND(level,     "!level",     UD_LEVEL);
COMMAND(flip,      "!flip",      UD_FLIP);
COMMAND(zero,      "!zero",      UD_ZERO);
COMMAND(newgame,   "!newgame",   UD_NEWGAME);
