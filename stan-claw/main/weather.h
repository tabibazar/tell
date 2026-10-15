#ifndef WEATHER_H
#define WEATHER_H

/*
 * The weather for the resting screen, from Open-Meteo (free, no key): now,
 * today and tomorrow, in degrees Celsius. weather_url builds the request for
 * a place; weather_parse reads the reply. Pure: host_tests/test_sc_weather.c.
 */
#include <stdbool.h>
#include <stddef.h>

typedef enum { WX_SUN, WX_PARTLY, WX_CLOUD, WX_FOG, WX_DRIZZLE, WX_RAIN, WX_SNOW, WX_THUNDER } wx_icon_t;

typedef struct {
    bool ok;
    float temp, feels;         /* now, degrees C */
    int code;                  /* WMO weather code, now */
    bool day;                  /* the sun is up (a moon instead of a sun at night) */
    float hi, lo;              /* today */
    int rain;                  /* today's chance of rain, percent */
    int t_code;                /* tomorrow */
    float t_hi, t_lo;
    int t_rain;
} weather_t;

/* "https://api.open-meteo.com/v1/forecast?latitude=..&longitude=..&..." into out. */
void weather_url(double lat, double lon, char *out, size_t n);
bool weather_parse(const char *json, weather_t *w);
const char *weather_words(int code);   /* "Partly cloudy", "Light rain" ... */
wx_icon_t weather_icon(int code);

#endif /* WEATHER_H */
