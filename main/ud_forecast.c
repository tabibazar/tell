#include "ud_internal.h"

#include <string.h>

/*
 * "!forecast": a pre-formatted line per day, tagged d0..d6, sent by the Mac
 * (tools/push-clock.sh). The board does not parse weather codes or numbers --
 * the Mac formats each day into one short line and the board just wraps it onto
 * a row. So a change to how a day reads is a change to the shell, not a reflash.
 *
 *   !forecast
 *   d0 Today  Rain    18/10  60%
 *   d1 Sun    Cloudy  20/12
 *   ...
 */
static void begin_forecast(usagedata_t *d, ud_host_t *h, int64_t now_us)
{
    (void)h; (void)now_us;
    for (int i = 0; i < UD_FC_DAYS; i++) d->forecast[i][0] = '\0';
}

static void line_forecast(usagedata_t *d, ud_host_t *h, const char *tag, const char *q)
{
    (void)h;
    if (tag[0] == 'd' && tag[1] >= '0' && tag[1] < '0' + UD_FC_DAYS && tag[2] == '\0')
        ud_text(q, d->forecast[tag[1] - '0'], UD_TEXT_MAX);
}

const ud_section_t ud_section_forecast = {
    "!forecast", UD_FORECAST, false,
    begin_forecast, line_forecast, NULL, NULL, NULL
};
