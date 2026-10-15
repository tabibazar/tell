/* The weather for the resting screen: Open-Meteo's reply read, and junk refused. */
#include "weather.h"
#include "tz.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    const char *reply = "{\"latitude\":43.76,\"current\":{\"time\":\"2026-10-14T21:00\",\"temperature_2m\":12.3,"
        "\"apparent_temperature\":9.4,\"weather_code\":3,\"is_day\":0},\"daily\":{\"time\":[\"2026-10-14\",\"2026-10-15\"],"
        "\"weather_code\":[51,63],\"temperature_2m_max\":[18.4,11.4],\"temperature_2m_min\":[11.6,7.6],"
        "\"precipitation_probability_max\":[17,26]}}";
    weather_t w;
    CHECK(weather_parse(reply, &w) && w.ok);
    CHECK(w.temp > 12.2f && w.temp < 12.4f && w.feels > 9.3f && w.code == 3 && !w.day);
    CHECK(w.hi > 18.3f && w.lo < 11.7f && w.rain == 17);
    CHECK(w.t_code == 63 && w.t_hi > 11.3f && w.t_lo < 7.7f && w.t_rain == 26);
    CHECK(strcmp(weather_words(3), "Cloudy") == 0 && weather_icon(63) == WX_RAIN && weather_icon(0) == WX_SUN);
    CHECK(weather_icon(75) == WX_SNOW && weather_icon(95) == WX_THUNDER && weather_icon(45) == WX_FOG);

    const char *nullrain = "{\"current\":{\"temperature_2m\":-3,\"apparent_temperature\":-9,\"weather_code\":71,\"is_day\":1},"
        "\"daily\":{\"weather_code\":[71,0],\"temperature_2m_max\":[-1,2],\"temperature_2m_min\":[-8,-6],"
        "\"precipitation_probability_max\":[null,null]}}";
    CHECK(weather_parse(nullrain, &w) && w.day && w.rain == 0 && w.temp < -2.9f);
    CHECK(!weather_parse("{\"error\":true,\"reason\":\"bad\"}", &w) && !w.ok);
    CHECK(!weather_parse("not json", &w));

    char url[400];
    weather_url(43.77, -79.41, url, sizeof url);
    CHECK(strstr(url, "latitude=43.77&longitude=-79.41") && strstr(url, "forecast_days=2"));

    /* Where the home is, from either service; refused on errors. */
    double la, lo;
    char city[40], tz[48];
    CHECK(weather_geo_parse("{\"ip\":\"x\",\"city\":\"Willowdale\",\"region\":\"Ontario\",\"loc\":\"43.7667,-79.3991\","
                            "\"timezone\":\"America/Toronto\"}", &la, &lo, city, sizeof city, tz, sizeof tz));
    CHECK(la > 43.76 && la < 43.77 && lo < -79.39 && lo > -79.40 && strcmp(city, "Willowdale") == 0);
    CHECK(strcmp(tz, "America/Toronto") == 0);
    CHECK(weather_geo_parse("{\"city\":\"Toronto\",\"latitude\":43.65,\"longitude\":-79.38}", &la, &lo, city, sizeof city, tz, sizeof tz));
    CHECK(la > 43.6 && lo < -79.3 && strcmp(city, "Toronto") == 0);
    CHECK(!weather_geo_parse("{\"error\":true,\"reason\":\"RateLimited\"}", &la, &lo, city, sizeof city, tz, sizeof tz) && city[0] == 0);
    CHECK(!weather_geo_parse("{\"loc\":\"0,0\"}", &la, &lo, city, sizeof city, tz, sizeof tz));
    CHECK(!weather_geo_parse("{\"bogon\":true}", &la, &lo, city, sizeof city, tz, sizeof tz));
    CHECK(!weather_geo_parse("<html>", &la, &lo, city, sizeof city, tz, sizeof tz));

    /* The zone's name to the C library's rule. */
    CHECK(tz_posix("America/Toronto") && strcmp(tz_posix("America/Toronto"), "EST5EDT,M3.2.0,M11.1.0") == 0);
    CHECK(tz_posix("Europe/London") && strcmp(tz_posix("Europe/London"), "GMT0BST,M3.5.0/1,M10.5.0") == 0);
    CHECK(tz_posix("Asia/Tehran") && strcmp(tz_posix("Asia/Tehran"), "<+0330>-3:30") == 0);
    CHECK(tz_posix("America/Los_Angeles") && strcmp(tz_posix("America/Los_Angeles"), "PST8PDT,M3.2.0,M11.1.0") == 0);
    CHECK(tz_posix("Africa/Abidjan") && tz_posix("Pacific/Wallis"));                 /* the first and last */
    CHECK(!tz_posix("Mars/Olympus") && !tz_posix("") && !tz_posix(NULL));

    printf(fails ? "%d FAILED\n" : "sc_weather: all passed\n", fails);
    return fails != 0;
}
