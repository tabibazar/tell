#include "ud_internal.h"

#include <stddef.h>

/*
 * The Feather's IMU pages are reached and adjusted by marker, like everything
 * else, but these markers carry no data: they are commands. They are sections
 * anyway so that every marker the board understands lives in one registry
 * rather than as special cases scattered through the parser, and main acts on
 * the kind that comes back.
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
COMMAND(level,     "!level",     UD_LEVEL);
COMMAND(game,      "!game",      UD_GAME);
COMMAND(flip,      "!flip",      UD_FLIP);
COMMAND(zero,      "!zero",      UD_ZERO);
