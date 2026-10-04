#!/bin/sh
# Flash tiny2 (generic ESP32-S3-DevKitC-1 N16R8 clone with an I2C camera) with the app in tiny2/build/.
#
#   tools/flash-tiny2.sh            app only, finds the CP2102 port
#   tools/flash-tiny2.sh --full     bootloader + partition table + app
#
# Use the board's CP2102 USB-C port (/dev/cu.usbserial-*), not the S3's own
# USB port: the factory MicroPython's USB port cannot be reset into the
# bootloader by esptool. 230400 baud: the CP2102 garbled reads at 460800+.
set -e
cd "$(dirname "$0")/.."

FULL=
ANY=
while : ; do
    case "$1" in
        --full) FULL=1; shift ;;
        --any)  ANY=--any; shift ;;
        *) break ;;
    esac
done

BIN=tiny2/build/tiny2.bin
[ -f "$BIN" ] || { echo "no $BIN; build first (cd tiny2 && idf.py build)"; exit 1; }
for src in tiny2/main/*.c tiny2/sdkconfig.defaults; do
    if [ "$src" -nt "$BIN" ]; then
        echo "$BIN is older than $src; the last build did not finish. Build first."
        exit 1
    fi
done

# blinky1 is a CP2102 too, so the port is no evidence: name it, or the
# MAC guard below sorts it out (it refuses blinky1).
PORT="${1:-$(ls /dev/cu.usbserial-* 2>/dev/null | grep -v usbserial-0001 | head -1)}"
[ -n "$PORT" ] || { echo "no CP2102 port: plug tiny2's UART/COM USB-C port in"; exit 1; }

. tools/board-guard.sh
board_guard tiny2 "$PORT" "$ANY"

if [ -n "$FULL" ]; then
    ARGS="0x0 tiny2/build/bootloader/bootloader.bin
          0x8000 tiny2/build/partition_table/partition-table.bin
          0x10000 $BIN"
else
    ARGS="0x10000 $BIN"
fi
echo "flashing $BIN to $PORT${FULL:+ (full)}"
# shellcheck disable=SC2086
"$HOME/.espressif/python_env/idf5.5_py3.13_env/bin/python" \
  "$HOME/esp/esp-idf/components/esptool_py/esptool/esptool.py" \
  --chip esp32s3 --port "$PORT" -b 230400 --after hard_reset write_flash $ARGS
echo "done"
