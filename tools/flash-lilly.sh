#!/bin/sh
# Flash lilly (LilyGO T-Display-S3) with the app built in build-lilly/.
#
#   tools/flash-lilly.sh                  app only, finds the usbmodem port
#   tools/flash-lilly.sh --full           bootloader + partition table + app
#   tools/flash-lilly.sh /dev/cu.usbmodem1101
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
case "$1" in
    --full) FULL=1; shift ;;
esac

BIN=build-lilly/screen.bin
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
    ARGS="0x0 build-lilly/bootloader/bootloader.bin
          0x8000 build-lilly/partition_table/partition-table.bin
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
DEVICE="${DEVICE:-lilly}"
# lilly has no RTC, so she starts at --:--:-- until a Mac tells her the time.
./tools/push-clock.sh "$DEVICE" || true
echo "done"
