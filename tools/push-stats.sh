#!/bin/sh
# Push both usage pages to a board. Two messages, because each marker
# replaces exactly one page. Data updates the board silently; it does not
# steal whatever page you are looking at.
set -e
cd "$(dirname "$0")/.."
DEVICE="${1:-big}"

# Runs on a one-minute timer, so refuse to overlap a slow previous run.
LOCK="/tmp/push-stats-$DEVICE.lock"
if ! mkdir "$LOCK" 2>/dev/null; then
    echo "another push to $DEVICE is still running" >&2
    exit 0
fi
trap 'rmdir "$LOCK"' EXIT

./tools/claude-stats.py --format data --section stats | ./tools/tell-locked.sh --device "$DEVICE"
./tools/claude-stats.py --format data --section daily | ./tools/tell-locked.sh --device "$DEVICE"
echo "pushed stats and daily to $DEVICE"
