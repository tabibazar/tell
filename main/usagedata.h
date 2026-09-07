#ifndef USAGEDATA_H
#define USAGEDATA_H

#include <stdint.h>

#define UD_MAX_MODELS 8
#define UD_MAX_DAYS   14
#define UD_NAME_MAX   15
#define UD_LABEL_MAX  7
#define UD_TEXT_MAX   63

typedef struct {
    char name[UD_NAME_MAX + 1];
    uint64_t out;
    uint64_t cread;
} ud_model_t;

typedef struct {
    char label[UD_LABEL_MAX + 1];
    uint64_t tokens;
} ud_day_t;

typedef struct {
    ud_model_t models[UD_MAX_MODELS];
    int model_count;
    ud_day_t days[UD_MAX_DAYS];
    int day_count;
    /* Sent from the Mac: the board knows only seconds since midnight, so it
       cannot work out a date, and it has no network to ask about weather. */
    char date[UD_TEXT_MAX + 1];
    char weather[UD_TEXT_MAX + 1];
} usagedata_t;

typedef enum { UD_NONE = 0, UD_STATS, UD_DAILY, UD_CLOCK } ud_kind_t;

/* Parses one payload. A payload starting with "!stats", "!daily" or "!clock"
   replaces that section wholesale; anything else returns UD_NONE and leaves
   `d` untouched, so the caller can treat it as a text message. */
ud_kind_t usagedata_parse(usagedata_t *d, const char *payload);

#endif /* USAGEDATA_H */
