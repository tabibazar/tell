#!/bin/sh
# Flash envo (NiceMCU-32S-DEV 2.8IPS) with the app built in build-envo/.
#
#   tools/flash-envo.sh                  app only, finds the usbmodem port
#   tools/flash-envo.sh --full           bootloader + partition table + app
#   tools/flash-envo.sh /dev/cu.usbmodem1101
#
# Use --full for the first flash: unlike the Feather there is no TinyUF2 to
# preserve here, and unlike the Feather's layout the partition table is ours,
# so it has to be written once. After that the app alone is enough.
#
# Refuses a screen.bin older than any source file: a failed build leaves the
# previous binary in place, and flashing that means debugging a bug you have
# already fixed.
#
# Unlike every other board here, envo is a plain ESP32 behind a UART bridge
# rather than native USB-Serial-JTAG -- so DTR/RTS auto-reset works, a faster
# baud rate is safe, and it appears as usbserial rather than usbmodem.
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

BIN=build-envo/screen.bin
[ -f "$BIN" ] || { echo "no $BIN; build first"; exit 1; }
for src in main/*.c main/*.h; do
    if [ "$src" -nt "$BIN" ]; then
        echo "$BIN is older than $src; the last build did not finish. Build first."
        exit 1
    fi
done

PORT="${1:-$(ls /dev/cu.usbserial* /dev/cu.wchusbserial* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "no serial port: is the board on USB?"; exit 1; }

# Which board is actually on that port. envo and wave both appear as
# /dev/cu.usbmodem*, so the port is no evidence, and these firmwares are not
# interchangeable.
. tools/board-guard.sh
board_guard envo "$PORT" "$ANY"

if [ -n "$FULL" ]; then
    ARGS="0x0 build-envo/bootloader/bootloader.bin
          0x8000 build-envo/partition_table/partition-table.bin
          0x10000 $BIN"
else
    ARGS="0x10000 $BIN"
fi

. tools/idf-env.sh
echo "flashing $BIN to $PORT${FULL:+ (full)}"
# shellcheck disable=SC2086
"$HOME/.espressif/python_env/idf5.5_py3.13_env/bin/python" \
  "$HOME/esp/esp-idf/components/esptool_py/esptool/esptool.py" \
  --chip esp32 --port "$PORT" --after hard_reset write_flash $ARGS

echo "flashed; waiting for the board to advertise"
sleep 6
DEVICE="${DEVICE:-envo}"
# envo has no clock chip, so she starts at --:--:-- until a Mac tells her.
./tools/push-clock.sh "$DEVICE" || true
echo "done"
