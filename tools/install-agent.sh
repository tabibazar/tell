#!/bin/sh
# Install the launchd agents that keep a board updated.
#
#   tools/install-agent.sh            the big panel, five agents
#   tools/install-agent.sh lilly      a small panel, two
#
# Which agents a board gets depends on which pages it has. The big panel
# renders thirteen data sections and a story; a 26x7 panel renders the clock,
# the limits and the all-time totals, so sending it the other ten would be
# parsing work for pages that do not exist on it.
#
#   big     tell-stats  thirteen sections every 5 min
#           push-now    the live section and the limits every 60s
#           push-clock  date and weather every 5 min
#           push-today  the almanac every 30 min
#           push-story  Claude's recap of the day every 30 min
#
#   other   push-clock  date and weather every 5 min
#           push-lilly  limits, models, cost and the year every 60s
#
# Labels carry the device name, so two boards can be fed from one Mac without
# one board's agents evicting the other's. Paths are written at install time,
# so this works from wherever the repo is checked out.
set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
DEVICE="${1:-big}"

mkdir -p "$HOME/Library/LaunchAgents"

emit() {                # label, script, device, interval
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
        <string>$3</string>
    </array>
    <key>StartInterval</key><integer>$4</integer>
    <key>RunAtLoad</key><true/>
    <key>StandardOutPath</key><string>/tmp/$1.log</string>
    <key>StandardErrorPath</key><string>/tmp/$1.err</string>
</dict>
</plist>
PLIST
}

install_one() {         # name, script, interval
    label="com.tabibazar.$1.$DEVICE"
    plist="$HOME/Library/LaunchAgents/$label.plist"
    emit "$1.$DEVICE" "$2" "$DEVICE" "$3" > "$plist"
    launchctl bootout "gui/$(id -u)/$label" 2>/dev/null || true
    launchctl bootstrap "gui/$(id -u)" "$plist"
    echo "installed $label -> $ROOT/tools/$2 $DEVICE"

    # Earlier versions used a label with no device in it, so a second board
    # silently replaced the first board's agent. Clear the old one out rather
    # than leave it running beside the new pair.
    old="com.tabibazar.$1"
    if launchctl print "gui/$(id -u)/$old" >/dev/null 2>&1; then
        launchctl bootout "gui/$(id -u)/$old" 2>/dev/null || true
        rm -f "$HOME/Library/LaunchAgents/$old.plist"
        echo "  (removed the old device-less $old)"
    fi
}

if [ "$DEVICE" = "big" ]; then
    install_one tell-stats  push-stats.sh  300
    install_one push-now    push-now.sh     60
    install_one push-clock  push-clock.sh  300
    install_one push-today  push-today.sh 1800
    install_one push-story  push-story.sh 1800
else
    install_one push-clock  push-clock.sh  300
    install_one push-lilly  push-lilly.sh   60
fi

echo
echo "logs:   /tmp/<name>.$DEVICE.log and .err"
echo "remove: launchctl bootout gui/$(id -u)/com.tabibazar.<name>.$DEVICE"
