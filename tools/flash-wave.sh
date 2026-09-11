#!/bin/sh
# Flash wave (Waveshare ESP32-S3-LCD-1.47B) with the app built in build-wave/.
#
#   tools/flash-wave.sh                  app only, finds the usbmodem port
#   tools/flash-wave.sh --full           bootloader + partition table + app
#   tools/flash-wave.sh /dev/cu.usbmodem1101
#
# Use --full for the first flash: there is no recovery bootloader to preserve
# and the partition table is ours, so it has to be written once. After that
# the app alone is enough.
#
# Refuses a screen.bin older than any source file: a failed build leaves the
# previous binary in place, and flashing that means debugging a bug you have
# already fixed.
#
# Never passes --baud: this board is native USB-Serial-JTAG, which ignores the
# rate and then drops the port.
set -e
cd "$(dirname "$0")/.."

FULL=
case "$1" in
    --full) FULL=1; shift ;;
esac

BIN=build-wave/screen.bin
[ -f "$BIN" ] || { echo "no $BIN; build first"; exit 1; }
for src in main/*.c main/*.h; do
    if [ "$src" -nt "$BIN" ]; then
        echo "$BIN is older than $src; the last build did not finish. Build first."
        exit 1
    fi
done

PORT="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "no serial port: is the board on USB?"; exit 1; }

if [ -n "$FULL" ]; then
    ARGS="0x0 build-wave/bootloader/bootloader.bin
          0x8000 build-wave/partition_table/partition-table.bin
          0x10000 $BIN"
else
    ARGS="0x10000 $BIN"
fi

. tools/idf-env.sh
echo "flashing $BIN to $PORT${FULL:+ (full)}"
# shellcheck disable=SC2086
"$HOME/.espressif/python_env/idf5.5_py3.13_env/bin/python" \
  "$HOME/esp/esp-idf/components/esptool_py/esptool/esptool.py" \
  --chip esp32s3 --port "$PORT" --after hard_reset write_flash $ARGS

echo "flashed"
# No clock push here. wave comes home to the sand, not to a clock, so sending
# her the clock-and-weather payload would only pull her off the page she
# exists to show. Send one by hand if you want it:
#   ./tools/push-clock.sh wave
echo "done"
