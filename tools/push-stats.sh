#!/bin/sh
# Push the usage pages to a board, every five minutes. Thirteen messages,
# because each marker replaces exactly one section: models (the bar and line
# pages), days, the year heatmap, the API-equivalent cost, the weekday-by-hour
# rhythm, today, projects, cache use, tools, thinking, records, the programs
# behind the Bash calls, and turn timing. Data updates the board silently; it
# does not steal whatever page you are looking at.
#
# The account's limits are not here: they come from the API rather than the
# transcripts and want to be fresher than five minutes, so push-now.sh sends
# them every minute alongside the live section.
set -e
cd "$(dirname "$0")/.."
DEVICE="${1:-big}"

# Every line this prints carries the time, because the question these logs get
# asked is "is the board current?" and a bare "pushed" cannot answer it.
say() { printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*"; }

# Runs on a five-minute timer, so refuse to overlap a slow previous run.
LOCK="/tmp/push-stats-$DEVICE.lock"
# A lock left by a killed run (a flash or a bootout mid-push) would stop
# every later run; break it once it is clearly abandoned.
if [ -d "$LOCK" ] && [ $(( $(date +%s) - $(stat -f %m "$LOCK" 2>/dev/null || echo 0) )) -gt 240 ]; then
    rmdir "$LOCK" 2>/dev/null || true
fi
if ! mkdir "$LOCK" 2>/dev/null; then
    echo "$(date '+%Y-%m-%d %H:%M:%S') another push to $DEVICE is still running" >&2
    exit 0
fi
trap 'rmdir "$LOCK"' EXIT

# One pass over the transcripts writes every section, then each is sent.
OUT=$(mktemp -d /tmp/claude-stats.XXXXXX)
trap 'rmdir "$LOCK"; rm -rf "$OUT"' EXIT
./tools/claude-stats.py --format data --all "$OUT"
for section in stats daily year cost rhythm now projects cache tools thinking records runs turns; do
    [ -s "$OUT/$section.txt" ] && ./tools/tell-locked.sh --device "$DEVICE" < "$OUT/$section.txt"
done
say "pushed thirteen sections to $DEVICE"
