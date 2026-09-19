#!/bin/sh
# Flash envio (Waveshare ESP32-S3-Touch-LCD-3.5B) with the app built in build-envio/.
#
#   tools/flash-envio.sh                  app only, finds the usbmodem port
#   tools/flash-envio.sh --full           bootloader + partition table + app
#   tools/flash-envio.sh /dev/cu.usbmodem1101
#
# Use --full for the first flash: unlike the Feather there is no TinyUF2 to
# preserve here, and unlike the Feather's layout the partition table is ours,
# so it has to be written once. After that the app alone is enough.
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
ANY=
while : ; do
    case "$1" in
        --full) FULL=1; shift ;;
        --any)  ANY=--any; shift ;;      # skip the board check; see board-guard.sh
        *) break ;;
    esac
done

BIN=build-envio/screen.bin
[ -f "$BIN" ] || { echo "no $BIN; build first"; exit 1; }
for src in main/*.c main/*.h; do
    if [ "$src" -nt "$BIN" ]; then
        echo "$BIN is older than $src; the last build did not finish. Build first."
        exit 1
    fi
done

PORT="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "no serial port: is the board on USB?"; exit 1; }

# Which board is actually on that port. envio and wave both appear as
# /dev/cu.usbmodem*, so the port is no evidence, and these firmwares are not
# interchangeable.
. tools/board-guard.sh
board_guard envio "$PORT" "$ANY"

if [ -n "$FULL" ]; then
    ARGS="0x0 build-envio/bootloader/bootloader.bin
          0x8000 build-envio/partition_table/partition-table.bin
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

echo "flashed; waiting for the board to advertise"
sleep 6
DEVICE="${DEVICE:-envio}"
# envio has a PCF85063 RTC on board, but it is not wired into the firmware yet,
# so for now she starts at --:--:-- until a Mac tells her the time, like lilly.
./tools/push-clock.sh "$DEVICE" || true
echo "done"
