#!/bin/sh
# Flash panel1 (Waveshare ESP32-S3-Touch-LCD-4B) with the app in panel1/build/.
#
#   tools/flash-panel1.sh            app only, finds the CH343 port
#   tools/flash-panel1.sh --full     bootloader + partition table + app
#
# Its USB is a CH343, which garbled esptool's reads at 460800 and above:
# 115200 is slow (a minute and a half for the app) but sure. BAUD=230400 to try.
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

BIN=panel1/build/panel1.bin
[ -f "$BIN" ] || { echo "no $BIN; build first (cd panel1 && idf.py build)"; exit 1; }
for src in panel1/main/*.c panel1/main/*.h panel1/sdkconfig.defaults; do
    if [ "$src" -nt "$BIN" ]; then
        echo "$BIN is older than $src; the last build did not finish. Build first."
        exit 1
    fi
done

PORT="${1:-$(ls /dev/cu.usbmodem5B91* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "no port: is panel1 on USB?"; exit 1; }

. tools/board-guard.sh
board_guard panel1 "$PORT" "$ANY"

if [ -n "$FULL" ]; then
    ARGS="0x0 panel1/build/bootloader/bootloader.bin
          0x8000 panel1/build/partition_table/partition-table.bin
          0x10000 $BIN"
else
    ARGS="0x10000 $BIN"
fi
echo "flashing $BIN to $PORT${FULL:+ (full)}"
# shellcheck disable=SC2086
"$HOME/.espressif/python_env/idf5.5_py3.13_env/bin/python" \
  "$HOME/esp/esp-idf/components/esptool_py/esptool/esptool.py" \
  --chip esp32s3 --port "$PORT" -b "${BAUD:-115200}" --after hard_reset write_flash $ARGS
echo "done"
