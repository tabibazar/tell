#!/bin/sh
# Send the date and current weather to the board's clock page.
#
# The board has no network and knows only seconds since midnight, so both the
# calendar date and the weather have to come from here.
#
# Location is set in tools/weather.py (North York by default); override with
# CLAUDE_WEATHER_LAT / CLAUDE_WEATHER_LON.
set -e
cd "$(dirname "$0")/.."
DEVICE="${1:-big}"

# Every line this prints carries the time, because the question these logs get
# asked is "is the board current?" and a bare line cannot answer it.
say() { printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*"; }
LOCK="/tmp/push-clock-$DEVICE.lock"
# A lock left by a killed run (a flash or a bootout mid-push) would stop
# every later run; break it once it is clearly abandoned.
if [ -d "$LOCK" ] && [ $(( $(date +%s) - $(stat -f %m "$LOCK" 2>/dev/null || echo 0) )) -gt 240 ]; then
    rmdir "$LOCK" 2>/dev/null || true
fi
if ! mkdir "$LOCK" 2>/dev/null; then exit 0; fi
trap 'rmdir "$LOCK"' EXIT

DATE=$(date '+%A %d %B %Y')
# The board knows only seconds since local midnight, so the offset from UTC in
# minutes and the zone's name come from here; they let it show UTC beside the
# local time in every title bar.
UTC_OFF=$(python3 -c 'import datetime as d; print(int(d.datetime.now().astimezone().utcoffset().total_seconds() // 60))')
TZ_NAME=$(date '+%Z')

WX=$(./tools/weather.py)

printf '!clock\ndate %s\nutc %s\ntz %s\nwx %s\n' "$DATE" "$UTC_OFF" "$TZ_NAME" "$WX" \
    | ./tools/tell-locked.sh --device "$DEVICE"
say "clock: $DATE | $WX"
