#!/usr/bin/env python3
"""Print the !today payload: moon, sun, the day's numbers, next holiday.

The board has no calendar, no ephemeris and no network, so all of this is
worked out here and pushed. Sunrise and sunset come from Open-Meteo at the
same coordinates as the weather; everything else is arithmetic.
"""
import datetime as dt
import json
import os
import subprocess
import sys

LAT = os.environ.get("CLAUDE_WEATHER_LAT", "43.7615")
LON = os.environ.get("CLAUDE_WEATHER_LON", "-79.4111")
TZ = os.environ.get("CLAUDE_WEATHER_TZ", "America/Toronto")

# Reference new moon and the synodic month, which is all the accuracy a
# phase name and a drawn disc need.
NEW_MOON = dt.datetime(2000, 1, 6, 18, 14)
SYNODIC = 29.530588853

PHASES = ["New moon", "Waxing crescent", "First quarter", "Waxing gibbous",
          "Full moon", "Waning gibbous", "Last quarter", "Waning crescent"]


def moon(now):
    age = ((now - NEW_MOON).total_seconds() / 86400.0) % SYNODIC
    phase = age / SYNODIC
    name = PHASES[int(phase * 8 + 0.5) % 8]
    return phase, name, age


def nth_weekday(year, month, weekday, n):
    """The nth <weekday> of a month; weekday 0 = Monday."""
    d = dt.date(year, month, 1)
    d += dt.timedelta((weekday - d.weekday()) % 7)
    return d + dt.timedelta(weeks=n - 1)


def easter(year):
    """Anonymous Gregorian algorithm, for Good Friday."""
    a, b, c = year % 19, year // 100, year % 100
    d, e = b // 4, b % 4
    f = (b + 8) // 25
    g = (b - f + 1) // 3
    h = (19 * a + b - d - g + 15) % 30
    i, k = c // 4, c % 4
    l = (32 + 2 * e + 2 * i - h - k) % 7
    m = (a + 11 * h + 22 * l) // 451
    month = (h + l - 7 * m + 114) // 31
    day = ((h + l - 7 * m + 114) % 31) + 1
    return dt.date(year, month, day)


def ontario_holidays(year):
    """The nine Ontario public holidays. The Civic Holiday in August is not
    one of them, so it is deliberately absent."""
    victoria = dt.date(year, 5, 25)
    while victoria.weekday() != 0:          # the Monday before 25 May
        victoria -= dt.timedelta(days=1)
    return [
        ("New Year's Day", dt.date(year, 1, 1)),
        ("Family Day", nth_weekday(year, 2, 0, 3)),
        ("Good Friday", easter(year) - dt.timedelta(days=2)),
        ("Victoria Day", victoria),
        ("Canada Day", dt.date(year, 7, 1)),
        ("Labour Day", nth_weekday(year, 9, 0, 1)),
        ("Thanksgiving", nth_weekday(year, 10, 0, 2)),
        ("Christmas Day", dt.date(year, 12, 25)),
        ("Boxing Day", dt.date(year, 12, 26)),
    ]


def next_holiday(today):
    upcoming = [(n, d) for n, d in ontario_holidays(today.year) if d >= today]
    if not upcoming:
        upcoming = ontario_holidays(today.year + 1)
    name, when = min(upcoming, key=lambda x: x[1])
    days = (when - today).days
    if days == 0:
        return "%s -- today" % name
    return "%s  %s  %d day%s" % (name, when.strftime("%a %d %b"), days,
                                 "" if days == 1 else "s")


def sun_times():
    url = ("https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
           "&daily=sunrise,sunset,daylight_duration&forecast_days=1"
           "&timezone=%s" % (LAT, LON, TZ))
    try:
        out = subprocess.run(["curl", "-sS", "--max-time", "15", url],
                             capture_output=True, text=True, timeout=20)
        d = json.loads(out.stdout)["daily"]
        rise = d["sunrise"][0][11:16]
        set_ = d["sunset"][0][11:16]
        hours = d["daylight_duration"][0] / 3600.0
        return "rise %s   set %s   %.1fh light" % (rise, set_, hours)
    except Exception as e:                      # noqa: BLE001
        print("almanac: %s" % e, file=sys.stderr)
        return ""


def main():
    now = dt.datetime.now()
    today = now.date()
    phase, name, age = moon(now)

    start = dt.date(today.year, 1, 1)
    end = dt.date(today.year, 12, 31)
    doy = today.timetuple().tm_yday
    total = end.timetuple().tm_yday
    left = (end - today).days

    print("!today")
    print("moon %.3f %s, %.0f days old" % (phase, name, age))
    sun = sun_times()
    if sun:
        print("sun %s" % sun)
    print("day %d of %d   %d left   week %s" % (doy, total, left,
                                                today.strftime("%V")))
    print("hol %s" % next_holiday(today))
    return 0


if __name__ == "__main__":
    sys.exit(main())
