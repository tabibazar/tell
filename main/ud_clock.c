#include "ud_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void begin_clock(usagedata_t *d, ud_host_t *h, int64_t now_us)
{
    (void)d; (void)h; (void)now_us;
    d->date[0] = '\0';
    d->weather[0] = '\0';
}

static void line_clock(usagedata_t *d, ud_host_t *h, const char *tag, const char *q)
{
    (void)d; (void)h;
    /* The rest of the line is free text, so take it verbatim. */
    char *dest = NULL;
    if (strcmp(tag, "date") == 0) dest = d->date;
    else if (strcmp(tag, "wx") == 0) dest = d->weather;
    if (dest == NULL) return;

    ud_text(q, dest, UD_TEXT_MAX);
}

const ud_section_t ud_section_clock = {
    "!clock", UD_CLOCK, false,
    begin_clock, line_clock, NULL, NULL, NULL
};

static void begin_today(usagedata_t *d, ud_host_t *h, int64_t now_us)
{
    (void)d; (void)h; (void)now_us;
    d->moon_name[0] = d->sun[0] = d->dayinfo[0] = d->holiday[0] = '\0';
    d->moon_phase = 0.0f;
}

static void line_today(usagedata_t *d, ud_host_t *h, const char *tag, const char *q)
{
    (void)d; (void)h;
    char *dest = NULL;
    if (strcmp(tag, "moon") == 0) {
        /* "moon <phase> <name>": a number then free text. */
        char num[16];
        const char *r = ud_token(q, num, sizeof num - 1);
        if (r == NULL) return;
        d->moon_phase = (float)atof(num);
        q = r;
        dest = d->moon_name;
    } else if (strcmp(tag, "sun") == 0) dest = d->sun;
    else if (strcmp(tag, "day") == 0) dest = d->dayinfo;
    else if (strcmp(tag, "hol") == 0) dest = d->holiday;
    if (dest == NULL) return;

    ud_text(q, dest, UD_TEXT_MAX);
}

const ud_section_t ud_section_today = {
    "!today", UD_TODAY, false,
    begin_today, line_today, NULL, NULL, NULL
};
