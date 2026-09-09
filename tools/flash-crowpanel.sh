#!/bin/sh
# Flash the CrowPanel with the app built in build-crowpanel/, app partition
# only (0x10000, never the bootloader or partition table), then push every
# data section so the pages have content straight away.
#
#   tools/flash-crowpanel.sh                 finds the usbserial port itself
#   tools/flash-crowpanel.sh /dev/cu.usbserial-210
#
# Refuses a screen.bin older than any source file: a failed build leaves the
# previous binary in place, and flashing that means debugging a bug you have
# already fixed. Pauses the stats agent while it works, so its pushes do not
# collide with the flash, and starts it again whatever happens.
set -e
cd "$(dirname "$0")/.."

AGENT="gui/$(id -u)/com.tabibazar.tell-stats"
PLIST="$HOME/Library/LaunchAgents/com.tabibazar.tell-stats.plist"
resume_agent() {
    if [ -n "$PAUSED" ] && [ -f "$PLIST" ]; then
        launchctl bootstrap "gui/$(id -u)" "$PLIST" 2>/dev/null && echo "stats agent resumed"
    fi
}
trap resume_agent EXIT

BIN=build-crowpanel/screen.bin
[ -f "$BIN" ] || { echo "no $BIN; build first"; exit 1; }
for src in main/*.c main/*.h; do
    if [ "$src" -nt "$BIN" ]; then
        echo "$BIN is older than $src; the last build did not finish. Build first."
        exit 1
    fi
done

PORT="${1:-$(ls /dev/cu.wchusbserial* /dev/cu.usbserial* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "no serial port: is the board on USB?"; exit 1; }

if launchctl print "$AGENT" >/dev/null 2>&1; then
    launchctl bootout "$AGENT" && PAUSED=1 && echo "stats agent paused"
    sleep 1
fi

. tools/idf-env.sh
echo "flashing $BIN to $PORT"
"$HOME/.espressif/python_env/idf5.5_py3.13_env/bin/python" \
  "$HOME/esp/esp-idf/components/esptool_py/esptool/esptool.py" \
  --chip esp32s3 --port "$PORT" --after hard_reset write_flash 0x10000 "$BIN"

echo "flashed; waiting for the board to advertise"
sleep 6
DEVICE="${DEVICE:-big}"
./tools/push-stats.sh "$DEVICE"
./tools/push-clock.sh "$DEVICE" || true
./tools/push-today.sh "$DEVICE" || true
echo "done"
