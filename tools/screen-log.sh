#!/bin/sh
# Append a line to the screen feed. Cheap: a file write, no BLE, no waiting.
# The pump ships it to the board a few seconds later.
LOG="${CLAUDE_SCREEN_LOG:-/tmp/claude-screen.log}"
if [ $# -gt 0 ]; then
    printf '%s\n' "$*" >> "$LOG"
else
    cat >> "$LOG"
fi
