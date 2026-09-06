#!/bin/sh
# Push both usage pages to a board. Two messages, because each marker
# replaces exactly one page.
set -e
cd "$(dirname "$0")/.."
DEVICE="${1:-big}"

./tools/claude-stats.py --format data --section stats | ./mac/tell --device "$DEVICE"
sleep 1
./tools/claude-stats.py --format data --section daily | ./mac/tell --device "$DEVICE"
echo "pushed stats and daily to $DEVICE"
