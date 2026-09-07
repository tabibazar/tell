#!/bin/sh
# Send today's story: a recap of the day written by Claude from the prompts.
# Regenerated only when the day has moved on; otherwise the cached text.
set -e
cd "$(dirname "$0")/.."
DEVICE="${1:-big}"

LOCK="/tmp/push-story-$DEVICE.lock"
if ! mkdir "$LOCK" 2>/dev/null; then exit 0; fi
trap 'rmdir "$LOCK"' EXIT

./tools/story.py | ./tools/tell-locked.sh --device "$DEVICE"
echo "story: $(./tools/story.py | grep -c '^text ') lines"
