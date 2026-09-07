#!/bin/sh
# One-shot setup for a Mac: build the client, put it on PATH, and start the
# agents that keep the board's clock, charts and almanac fresh.
#
#   ./install.sh            set everything up for the board named "big"
#   ./install.sh small      ... or for the small one
#   ./install.sh big --no-agents    client only, no background pushing
set -e
cd "$(dirname "$0")"
ROOT="$(pwd)"
DEVICE="big"
AGENTS=yes

for arg in "$@"; do
    case "$arg" in
        --no-agents) AGENTS=no ;;
        -*) echo "unknown option: $arg" >&2; exit 2 ;;
        *) DEVICE="$arg" ;;
    esac
done

say() { printf '\n== %s\n' "$1"; }
die() { printf 'error: %s\n' "$1" >&2; exit 1; }

say "Checking what this Mac has"
[ "$(uname -s)" = "Darwin" ] || die "this only runs on macOS"
command -v swiftc >/dev/null || die "swiftc missing -- run: xcode-select --install"
command -v curl   >/dev/null || die "curl missing"
command -v python3 >/dev/null || die "python3 missing"
echo "swiftc, curl and python3 present"

say "Building the client"
./mac/build.sh

say "Putting tell on your PATH"
# ~/.local/bin needs no sudo and is on the default PATH on most setups.
BIN="$HOME/.local/bin"
mkdir -p "$BIN"
ln -sf "$ROOT/mac/tell" "$BIN/tell"
echo "linked $BIN/tell -> $ROOT/mac/tell"
case ":$PATH:" in
    *":$BIN:"*) ;;
    *) echo "NOTE: $BIN is not on your PATH."
       echo "      add this to ~/.zshrc:  export PATH=\"\$HOME/.local/bin:\$PATH\"" ;;
esac

say "Looking for the boards"
# The first run is also what triggers the Bluetooth permission prompt.
if ! "$ROOT/mac/tell" --list; then
    echo
    echo "No board answered. Either none is powered up and in range, or macOS"
    echo "has not been granted Bluetooth access for this terminal:"
    echo "  System Settings > Privacy & Security > Bluetooth > enable your terminal"
    echo "Then run ./install.sh again."
fi

if [ "$AGENTS" = yes ]; then
    say "Starting the background agents for '$DEVICE'"
    ./tools/install-agent.sh "$DEVICE"
    echo
    echo "This machine will now contribute its Claude usage to the charts."
    echo "The board keeps each machine's data separate and shows the total."
else
    say "Skipping the agents (--no-agents)"
fi

say "Done"
cat <<USAGE
  tell --list                        which boards are in range
  tell --device $DEVICE "hello"          send a message
  echo hi | tell --device $DEVICE        stdin works too
  tell --device $DEVICE ""               clear the message
  tell --device $DEVICE --sync           set the board's clock

To stop the agents later:
  launchctl bootout gui/$(id -u)/com.tabibazar.tell-stats
  launchctl bootout gui/$(id -u)/com.tabibazar.push-clock
  launchctl bootout gui/$(id -u)/com.tabibazar.push-today
  launchctl bootout gui/$(id -u)/com.tabibazar.push-story
USAGE
