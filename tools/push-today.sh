#!/bin/sh
# Send the almanac page: moon, sun, the day's numbers and the next holiday.
set -e
cd "$(dirname "$0")/.."
DEVICE="${1:-big}"

# Every line this prints carries the time, because the question these logs get
# asked is "is the board current?" and a bare line cannot answer it.
say() { printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*"; }

LOCK="/tmp/push-today-$DEVICE.lock"
# A lock left by a killed run (a flash or a bootout mid-push) would stop
# every later run; break it once it is clearly abandoned.
if [ -d "$LOCK" ] && [ $(( $(date +%s) - $(stat -f %m "$LOCK" 2>/dev/null || echo 0) )) -gt 240 ]; then
    rmdir "$LOCK" 2>/dev/null || true
fi
if ! mkdir "$LOCK" 2>/dev/null; then exit 0; fi
trap 'rmdir "$LOCK"' EXIT

./tools/almanac.py | ./tools/tell-locked.sh --device "$DEVICE"
say "today: $(./tools/almanac.py | tail -1)"
