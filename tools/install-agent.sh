#!/bin/sh
# Installs the launchd agent that refreshes the board's stats every minute.
set -e
cd "$(dirname "$0")/.."
mkdir -p "$HOME/Library/LaunchAgents"

# tell-stats refreshes the charts; screen-pump ships the running log.
for name in tell-stats screen-pump push-clock; do
    label="com.tabibazar.$name"
    plist="$HOME/Library/LaunchAgents/$label.plist"
    cp "tools/$label.plist" "$plist"
    launchctl bootout "gui/$(id -u)/$label" 2>/dev/null || true
    launchctl bootstrap "gui/$(id -u)" "$plist"
    echo "installed $label"
done
echo "logs: /tmp/tell-stats.log /tmp/screen-pump.log /tmp/push-clock.log (and .err)"
echo "remove: launchctl bootout gui/$(id -u)/com.tabibazar.<name>"
