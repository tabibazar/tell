#!/bin/sh
# Ship whatever is new in the feed to the board, then remember how far we got.
# Runs on a short timer, so batching keeps the radio use sane: one BLE
# connection per tick regardless of how many lines arrived.
set -e
cd "$(dirname "$0")/.."

LOG="${CLAUDE_SCREEN_LOG:-/tmp/claude-screen.log}"
OFFSET="$LOG.offset"
DEVICE="${1:-big}"
LOCK="/tmp/screen-pump-$DEVICE.lock"

[ -f "$LOG" ] || exit 0
if ! mkdir "$LOCK" 2>/dev/null; then exit 0; fi
trap 'rmdir "$LOCK"' EXIT

sent=$(cat "$OFFSET" 2>/dev/null || echo 0)
total=$(wc -l < "$LOG" | tr -d ' ')

# The log was truncated or replaced, so start over.
[ "$total" -lt "$sent" ] && sent=0
[ "$total" -eq "$sent" ] && exit 0

# The board holds 48 lines, so never ship more than that in one go.
new=$((total - sent))
[ "$new" -gt 40 ] && new=40

tail -n "$new" "$LOG" | ./mac/tell --device "$DEVICE"
echo "$total" > "$OFFSET"
