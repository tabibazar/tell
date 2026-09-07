#!/bin/sh
# Installs the launchd agent that refreshes the board's stats every minute.
set -e
cd "$(dirname "$0")/.."
PLIST="$HOME/Library/LaunchAgents/com.tabibazar.tell-stats.plist"

mkdir -p "$HOME/Library/LaunchAgents"
cp tools/com.tabibazar.tell-stats.plist "$PLIST"

launchctl bootout "gui/$(id -u)/com.tabibazar.tell-stats" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST"
echo "installed; logs in /tmp/tell-stats.log and /tmp/tell-stats.err"
echo "remove with: launchctl bootout gui/$(id -u)/com.tabibazar.tell-stats"
