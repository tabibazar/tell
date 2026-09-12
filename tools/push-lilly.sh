#!/bin/sh
# The numbers lilly's two data pages draw, every minute.
#
# Only three sections, because her panel holds only three: the limits, which
# her Limits page counts down between pushes; the model totals and the API
# cost, which her Usage page adds up; and the year grid, which is where the
# active-days count comes from. The other ten sections push-stats.sh sends to
# the big panel have no page on a 26x7 screen and would be parsed and thrown
# away.
#
# The clock and the weather are not here. They come from push-clock.sh on its
# own five-minute timer: weather.py has no cache, and asking a weather service
# sixty times an hour for a number that changes hourly is rude.
set -e
cd "$(dirname "$0")/.."
DEVICE="${1:-lilly}"

# Every line this prints carries the time, because the question these logs get
# asked is "is the board current?" and a bare "pushed" cannot answer it.
say() { printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*"; }

LOCK="/tmp/push-lilly-$DEVICE.lock"
# A lock left by a killed run (a flash or a bootout mid-push) would stop
# every later run; break it once it is clearly abandoned.
if [ -d "$LOCK" ] && [ $(( $(date +%s) - $(stat -f %m "$LOCK" 2>/dev/null || echo 0) )) -gt 240 ]; then
    rmdir "$LOCK" 2>/dev/null || true
fi
if ! mkdir "$LOCK" 2>/dev/null; then exit 0; fi

OUT=$(mktemp -d /tmp/lilly-stats.XXXXXX)
trap 'rmdir "$LOCK" 2>/dev/null; rm -rf "$OUT"' EXIT

# The limits come from the API and the rest from the transcripts, so they are
# two calls no matter what; --all makes the second one a single pass.
./tools/claude-stats.py --format data --section limits > "$OUT/limits.txt"
./tools/claude-stats.py --format data --all "$OUT"

sent=0
for section in limits stats cost year; do
    if [ -s "$OUT/$section.txt" ]; then
        ./tools/tell-locked.sh --device "$DEVICE" < "$OUT/$section.txt"
        sent=$((sent + 1))
    fi
done
say "pushed $sent of 4 sections to $DEVICE"
