#!/bin/sh
# Flash blinky1 (generic ESP32-S3-DevKitC-1 N16R8) with the app in blinky1/build/.
#
#   tools/flash-blinky1.sh            app only, finds the CP2102 port
#   tools/flash-blinky1.sh --full     bootloader + partition table + app
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

BIN=blinky1/build/blinky1.bin
[ -f "$BIN" ] || { echo "no $BIN; build first (cd blinky1 && idf.py build)"; exit 1; }
for src in blinky1/main/*.c main/lightlink.h blinky1/sdkconfig.defaults; do
    if [ "$src" -nt "$BIN" ]; then
        echo "$BIN is older than $src; the last build did not finish. Build first."
        exit 1
    fi
done

PORT="${1:-$(ls /dev/cu.usbserial-* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "no CP2102 port: plug blinky1's UART/COM USB-C port in"; exit 1; }

. tools/board-guard.sh
board_guard blinky1 "$PORT" "$ANY"

if [ -n "$FULL" ]; then
    ARGS="0x0 blinky1/build/bootloader/bootloader.bin
          0x8000 blinky1/build/partition_table/partition-table.bin
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
