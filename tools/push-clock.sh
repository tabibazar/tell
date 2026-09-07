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
LOCK="/tmp/push-clock-$DEVICE.lock"
if ! mkdir "$LOCK" 2>/dev/null; then exit 0; fi
trap 'rmdir "$LOCK"' EXIT

DATE=$(date '+%A %d %B %Y')

WX=$(./tools/weather.py)

printf '!clock\ndate %s\nwx %s\n' "$DATE" "$WX" | ./tools/tell-locked.sh --device "$DEVICE"
echo "clock: $DATE | $WX"
