#!/bin/sh
# Send the almanac page: moon, sun, the day's numbers and the next holiday.
set -e
cd "$(dirname "$0")/.."
DEVICE="${1:-big}"

LOCK="/tmp/push-today-$DEVICE.lock"
if ! mkdir "$LOCK" 2>/dev/null; then exit 0; fi
trap 'rmdir "$LOCK"' EXIT

./tools/almanac.py | ./tools/tell-locked.sh --device "$DEVICE"
echo "today: $(./tools/almanac.py | tail -1)"
