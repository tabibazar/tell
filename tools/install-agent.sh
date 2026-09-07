#!/bin/sh
# Install the launchd agents that keep the board updated:
#   tell-stats   pushes this machine's Claude usage every 5 min
#   push-now     pushes just the "today, live" section every 60s
#   push-clock   sends the date and weather every 5 min
#   push-today   sends the almanac every 30 min
#   push-story   has Claude write a recap of the day every 30 min
#
# Paths are written at install time, so this works from wherever the repo is
# checked out -- including a second Mac.
set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
DEVICE="${1:-big}"

mkdir -p "$HOME/Library/LaunchAgents"

emit() {                # name, script, interval
    cat <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>com.tabibazar.$1</string>
    <key>ProgramArguments</key>
    <array>
        <string>$ROOT/tools/$2</string>
        <string>$DEVICE</string>
    </array>
    <key>StartInterval</key><integer>$3</integer>
    <key>RunAtLoad</key><true/>
    <key>StandardOutPath</key><string>/tmp/$1.log</string>
    <key>StandardErrorPath</key><string>/tmp/$1.err</string>
</dict>
</plist>
PLIST
}

install_one() {         # name, script, interval
    label="com.tabibazar.$1"
    plist="$HOME/Library/LaunchAgents/$label.plist"
    emit "$1" "$2" "$3" > "$plist"
    launchctl bootout "gui/$(id -u)/$label" 2>/dev/null || true
    launchctl bootstrap "gui/$(id -u)" "$plist"
    echo "installed $label -> $ROOT/tools/$2 $DEVICE"
}

install_one tell-stats  push-stats.sh  300
install_one push-now    push-now.sh     60
install_one push-clock  push-clock.sh  300
install_one push-today  push-today.sh 1800
install_one push-story  push-story.sh 1800

echo "logs: /tmp/tell-stats.log /tmp/push-now.log /tmp/push-clock.log /tmp/push-today.log /tmp/push-story.log (and .err)"
echo "remove: launchctl bootout gui/$(id -u)/com.tabibazar.<name>"
