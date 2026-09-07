#!/usr/bin/env python3
"""Print one ASCII line of current weather for the board.

Defaults to North York, Toronto. Override with CLAUDE_WEATHER_LAT / _LON,
and the label with CLAUDE_WEATHER_NAME.

Open-Meteo needs no API key and takes exact coordinates, which matters here:
an IP lookup resolves to downtown Toronto and reported 15C when North York
was 13.1C. Output is ASCII only -- the screen font covers 32..126, so a
degree sign would render as '?'.
"""
import json
import os
import subprocess
import sys

LAT = os.environ.get("CLAUDE_WEATHER_LAT", "43.7615")
LON = os.environ.get("CLAUDE_WEATHER_LON", "-79.4111")
TZ = os.environ.get("CLAUDE_WEATHER_TZ", "America/Toronto")

URL = ("https://api.open-meteo.com/v1/forecast"
       "?latitude=%s&longitude=%s"
       "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
       "precipitation,weather_code,wind_speed_10m"
       "&daily=temperature_2m_max,temperature_2m_min"
       "&forecast_days=1&timezone=%s") % (LAT, LON, TZ)

# WMO weather codes, kept short enough for a 62-column line.
CODES = {
    0: "Clear", 1: "Mainly clear", 2: "Partly cloudy", 3: "Overcast",
    45: "Fog", 48: "Rime fog",
    51: "Light drizzle", 53: "Drizzle", 55: "Heavy drizzle",
    56: "Freezing drizzle", 57: "Freezing drizzle",
    61: "Light rain", 63: "Rain", 65: "Heavy rain",
    66: "Freezing rain", 67: "Freezing rain",
    71: "Light snow", 73: "Snow", 75: "Heavy snow", 77: "Snow grains",
    80: "Light showers", 81: "Showers", 82: "Heavy showers",
    85: "Snow showers", 86: "Snow showers",
    95: "Thunderstorm", 96: "Thunderstorm hail", 99: "Thunderstorm hail",
}


def main():
    # curl rather than urllib: this machine's Python has no usable CA bundle
    # and urlopen fails with CERTIFICATE_VERIFY_FAILED.
    try:
        out = subprocess.run(["curl", "-sS", "--max-time", "15", URL],
                             capture_output=True, text=True, timeout=20)
        d = json.loads(out.stdout)
    except Exception as e:                      # noqa: BLE001 - any failure is the same
        print("weather unavailable")
        print("weather: %s" % e, file=sys.stderr)
        return 0

    # An API error still parses as JSON, so a bad location would otherwise
    # render as "? 0.0C" rather than admitting it failed.
    if d.get("error") or not isinstance(d.get("current"), dict):
        print("weather unavailable")
        print("weather: %s" % (d.get("reason") or "no current data"),
              file=sys.stderr)
        return 0

    cur = d["current"]
    daily = d.get("daily", {})

    desc = CODES.get(cur.get("weather_code"), "?")
    parts = ["%s %.1fC" % (desc, cur.get("temperature_2m", 0))]

    feels = cur.get("apparent_temperature")
    if feels is not None and abs(feels - cur.get("temperature_2m", 0)) >= 0.5:
        parts.append("feels %.0fC" % feels)

    hum = cur.get("relative_humidity_2m")
    if hum is not None:
        parts.append("%d%%" % hum)

    wind = cur.get("wind_speed_10m")
    if wind is not None:
        parts.append("%.0fkm/h" % wind)

    rain = cur.get("precipitation") or 0
    if rain > 0:
        parts.append("%.1fmm" % rain)

    try:
        hi = daily["temperature_2m_max"][0]
        lo = daily["temperature_2m_min"][0]
        parts.append("hi %.0f lo %.0f" % (hi, lo))
    except (KeyError, IndexError, TypeError):
        pass

    line = "  ".join(parts)
    print("".join(c for c in line if 32 <= ord(c) <= 126)[:62])
    return 0


if __name__ == "__main__":
    sys.exit(main())
