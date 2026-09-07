#!/bin/sh
# Send the date and current weather to the board's clock page.
#
# The board has no network and knows only seconds since midnight, so both the
# calendar date and the weather have to come from here.
#
# Set CLAUDE_SCREEN_CITY to pin a location; otherwise wttr.in geolocates by IP.
set -e
cd "$(dirname "$0")/.."
DEVICE="${1:-big}"
CITY="${CLAUDE_SCREEN_CITY:-}"

LOCK="/tmp/push-clock-$DEVICE.lock"
if ! mkdir "$LOCK" 2>/dev/null; then exit 0; fi
trap 'rmdir "$LOCK"' EXIT

DATE=$(date '+%A %d %B %Y')

# The screen font is ASCII 32..126, so the degree sign must go. Keep it short:
# the panel is 64 columns.
WX=$(curl -s --max-time 15 "https://wttr.in/${CITY}?format=%C++%t++feels+%f++%h" 2>/dev/null \
     | tr -d '+' | sed -e 's/°C/C/g' -e 's/°F/F/g' -e 's/  */ /g' -e 's/^ //' -e 's/ $//' \
     | LC_ALL=C tr -cd '\40-\176' | cut -c1-62)

# A failed fetch must not blank the line and leave the page looking broken.
[ -n "$WX" ] || WX="weather unavailable"

printf '!clock\ndate %s\nwx %s\n' "$DATE" "$WX" | ./tools/tell-locked.sh --device "$DEVICE"
echo "clock: $DATE | $WX"
