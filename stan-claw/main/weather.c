#include "weather.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

void weather_url(double lat, double lon, char *out, size_t n)
{
    snprintf(out, n, "https://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f"
             "&current=temperature_2m,apparent_temperature,weather_code,is_day"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             "&timezone=auto&forecast_days=2", lat, lon);
}

static bool num(const cJSON *o, const char *k, double *v)
{
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsNumber(x)) return false;
    *v = x->valuedouble;
    return true;
}

static bool day_num(const cJSON *daily, const char *k, int i, double *v)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(daily, k);
    const cJSON *x = cJSON_GetArrayItem(a, i);
    if (!cJSON_IsNumber(x)) return false;
    *v = x->valuedouble;
    return true;
}

bool weather_parse(const char *json, weather_t *w)
{
    memset(w, 0, sizeof *w);
    cJSON *j = cJSON_Parse(json);
    if (!j) return false;
    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(j, "current");
    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(j, "daily");
    double t, f, c, d, hi, lo, r = 0, tc, thi, tlo, tr = 0;
    bool ok = num(cur, "temperature_2m", &t) && num(cur, "apparent_temperature", &f) && num(cur, "weather_code", &c)
           && day_num(daily, "temperature_2m_max", 0, &hi) && day_num(daily, "temperature_2m_min", 0, &lo)
           && day_num(daily, "weather_code", 1, &tc) && day_num(daily, "temperature_2m_max", 1, &thi)
           && day_num(daily, "temperature_2m_min", 1, &tlo);
    if (ok) {
        day_num(daily, "precipitation_probability_max", 0, &r);      /* sometimes null: then 0 */
        day_num(daily, "precipitation_probability_max", 1, &tr);
        w->temp = (float)t; w->feels = (float)f; w->code = (int)c;
        w->day = !num(cur, "is_day", &d) || d != 0;
        w->hi = (float)hi; w->lo = (float)lo; w->rain = (int)r;
        w->t_code = (int)tc; w->t_hi = (float)thi; w->t_lo = (float)tlo; w->t_rain = (int)tr;
        w->ok = true;
    }
    cJSON_Delete(j);
    return ok;
}

const char *weather_words(int c)
{
    if (c == 0) return "Clear";
    if (c == 1) return "Mainly clear";
    if (c == 2) return "Partly cloudy";
    if (c == 3) return "Cloudy";
    if (c == 45 || c == 48) return "Fog";
    if (c >= 51 && c <= 57) return "Drizzle";
    if (c == 61 || c == 80) return "Light rain";
    if (c == 63 || c == 81) return "Rain";
    if (c == 65 || c == 82) return "Heavy rain";
    if (c == 66 || c == 67) return "Freezing rain";
    if (c == 71 || c == 85) return "Light snow";
    if (c == 73 || c == 75 || c == 77 || c == 86) return "Snow";
    if (c >= 95) return "Thunderstorms";
    return "Mixed";
}

wx_icon_t weather_icon(int c)
{
    if (c <= 1) return WX_SUN;
    if (c == 2) return WX_PARTLY;
    if (c == 3) return WX_CLOUD;
    if (c == 45 || c == 48) return WX_FOG;
    if (c >= 51 && c <= 57) return WX_DRIZZLE;
    if ((c >= 61 && c <= 67) || (c >= 80 && c <= 82)) return WX_RAIN;
    if ((c >= 71 && c <= 77) || c == 85 || c == 86) return WX_SNOW;
    if (c >= 95) return WX_THUNDER;
    return WX_CLOUD;
}

bool weather_geo_parse(const char *json, double *lat, double *lon, char *city, size_t n, char *tz, size_t ntz)
{
    cJSON *j = cJSON_Parse(json);
    if (!j) return false;
    bool ok = false;
    const cJSON *loc = cJSON_GetObjectItemCaseSensitive(j, "loc");
    const cJSON *la = cJSON_GetObjectItemCaseSensitive(j, "latitude");
    const cJSON *lo = cJSON_GetObjectItemCaseSensitive(j, "longitude");
    if (cJSON_IsString(loc) && sscanf(loc->valuestring, "%lf,%lf", lat, lon) == 2) ok = true;
    else if (cJSON_IsNumber(la) && cJSON_IsNumber(lo)) { *lat = la->valuedouble; *lon = lo->valuedouble; ok = true; }
    if (ok && (*lat < -90 || *lat > 90 || *lon < -180 || *lon > 180 || (*lat == 0 && *lon == 0))) ok = false;
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(j, "city");
    if (n) snprintf(city, n, "%s", ok && cJSON_IsString(c) ? c->valuestring : "");
    const cJSON *z = cJSON_GetObjectItemCaseSensitive(j, "timezone");
    if (ntz) snprintf(tz, ntz, "%s", ok && cJSON_IsString(z) ? z->valuestring : "");
    cJSON_Delete(j);
    return ok;
}
