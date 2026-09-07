#!/bin/sh
# Push the usage pages to a board, every five minutes. Fourteen messages,
# because each marker replaces exactly one section: models (the bar and line pages), days, the
# year heatmap, the API-equivalent cost, the weekday-by-hour rhythm, today,
# projects, cache use, tools, thinking, this week against last, records, the
# programs behind the Bash calls, and turn timing. Data updates the board silently; it does not
# steal whatever page you are looking at.
set -e
cd "$(dirname "$0")/.."
DEVICE="${1:-big}"

# Runs on a five-minute timer, so refuse to overlap a slow previous run.
LOCK="/tmp/push-stats-$DEVICE.lock"
if ! mkdir "$LOCK" 2>/dev/null; then
    echo "another push to $DEVICE is still running" >&2
    exit 0
fi
trap 'rmdir "$LOCK"' EXIT

# One pass over the transcripts writes every section, then each is sent.
OUT=$(mktemp -d /tmp/claude-stats.XXXXXX)
trap 'rmdir "$LOCK"; rm -rf "$OUT"' EXIT
./tools/claude-stats.py --format data --all "$OUT"
for section in stats daily year cost rhythm now projects cache tools thinking week records runs turns; do
    [ -s "$OUT/$section.txt" ] && ./tools/tell-locked.sh --device "$DEVICE" < "$OUT/$section.txt"
done
echo "pushed fourteen sections to $DEVICE"
