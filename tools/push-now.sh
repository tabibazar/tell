#!/bin/sh
# Send the two sections that want to be fresh, every minute: "today, live",
# which is what tells the board whether Claude is busy right now, and the
# account's limits. The heavier sections go every five minutes from
# push-stats.sh.
#
# The limits' countdowns keep running on the board between pushes, so this is
# about the percentages moving, not about the clock.
set -e
cd "$(dirname "$0")/.."
DEVICE="${1:-big}"

# Every line this prints carries the time, because the question these logs get
# asked is "is the board current?" and a bare "pushed" cannot answer it.
say() { printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*"; }

LOCK="/tmp/push-now-$DEVICE.lock"
# A lock left by a killed run (a flash or a bootout mid-push) would stop
# every later run; break it once it is clearly abandoned.
if [ -d "$LOCK" ] && [ $(( $(date +%s) - $(stat -f %m "$LOCK" 2>/dev/null || echo 0) )) -gt 240 ]; then
    rmdir "$LOCK" 2>/dev/null || true
fi
if ! mkdir "$LOCK" 2>/dev/null; then exit 0; fi
trap 'rmdir "$LOCK"' EXIT

./tools/claude-stats.py --format data --section now | ./tools/tell-locked.sh --device "$DEVICE"
./tools/claude-stats.py --format data --section limits | ./tools/tell-locked.sh --device "$DEVICE"
say "pushed now and limits to $DEVICE"
