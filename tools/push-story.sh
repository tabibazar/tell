#!/bin/sh
# Send today's story: a recap of the day written by Claude from the prompts.
# Regenerated only when the day has moved on; otherwise the cached text.
set -e
cd "$(dirname "$0")/.."
DEVICE="${1:-big}"

LOCK="/tmp/push-story-$DEVICE.lock"
# A lock left by a killed run (a flash or a bootout mid-push) would stop
# every later run; break it once it is clearly abandoned.
if [ -d "$LOCK" ] && [ $(( $(date +%s) - $(stat -f %m "$LOCK" 2>/dev/null || echo 0) )) -gt 240 ]; then
    rmdir "$LOCK" 2>/dev/null || true
fi
if ! mkdir "$LOCK" 2>/dev/null; then exit 0; fi
trap 'rmdir "$LOCK"' EXIT

./tools/story.py | ./tools/tell-locked.sh --device "$DEVICE"
echo "story: $(./tools/story.py | grep -c '^text ') lines"
