#!/bin/sh
# Send only the "today, live" section, every minute. The thirteen heavier
# sections go every five minutes from push-stats.sh; this one is small and is
# what tells the board whether Claude is busy right now.
set -e
cd "$(dirname "$0")/.."
DEVICE="${1:-big}"

LOCK="/tmp/push-now-$DEVICE.lock"
# A lock left by a killed run (a flash or a bootout mid-push) would stop
# every later run; break it once it is clearly abandoned.
if [ -d "$LOCK" ] && [ $(( $(date +%s) - $(stat -f %m "$LOCK" 2>/dev/null || echo 0) )) -gt 240 ]; then
    rmdir "$LOCK" 2>/dev/null || true
fi
if ! mkdir "$LOCK" 2>/dev/null; then exit 0; fi
trap 'rmdir "$LOCK"' EXIT

./tools/claude-stats.py --format data --section now | ./tools/tell-locked.sh --device "$DEVICE"
echo "pushed now to $DEVICE"
