#!/bin/sh
# Flash speaker (Waveshare ESP32-S3-AUDIO-Board) with the app built in build-speaker/.
#
#   tools/flash-speaker.sh                  app only, finds the usbmodem port
#   tools/flash-speaker.sh --full           bootloader + partition table + app
#   tools/flash-speaker.sh /dev/cu.usbmodem1101
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
# rate and then drops the port. It stays on USB only while the TCA9555's EXIO6
# (Camera_SEL) is high, which is why the firmware never drives that pin: low,
# it hands GPIO19/20 to the camera connector and every flash after that needs
# BOOT held through a RESET.
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

BIN=build-speaker/screen.bin
[ -f "$BIN" ] || { echo "no $BIN; build first"; exit 1; }
for src in main/*.c main/*.h; do
    if [ "$src" -nt "$BIN" ]; then
        echo "$BIN is older than $src; the last build did not finish. Build first."
        exit 1
    fi
done

PORT="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "no serial port: is the board on USB?"; exit 1; }

# Which board is actually on that port. Every S3 board here appears as
# /dev/cu.usbmodem*, so the port is no evidence, and these firmwares are not
# interchangeable.
. tools/board-guard.sh
board_guard speaker "$PORT" "$ANY"

if [ -n "$FULL" ]; then
    ARGS="0x0 build-speaker/bootloader/bootloader.bin
          0x8000 build-speaker/partition_table/partition-table.bin
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
# The PCF85063 keeps the time once set; pushing it here sets or corrects it
# from the Mac after every flash, which is also how it is set the first time.
# Nothing is written to the card until speaker has a date, so this is also
# what starts the SD log on a board whose clock cell is flat or missing.
./tools/push-clock.sh "${DEVICE:-speaker}" || true
echo "done"
