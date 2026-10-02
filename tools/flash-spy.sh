#!/bin/sh
# Flash spy (Waveshare ESP32-S3-SIM7670G-4G) with the app built in spy/build/.
#
#   tools/flash-spy.sh                  app only, finds the CH343 port
#   tools/flash-spy.sh --full           bootloader + partition table + app
#   tools/flash-spy.sh /dev/cu.usbmodem5B910319591
#
# spy's ESP32 is reached only through its CH343 UART bridge (the S3's own USB
# never reaches the USB-C), so unlike speaker a baud rate is fine here. The
# modem's four USB ports (/dev/cu.usbmodem000000000001x, with the "USB" DIP
# on) are not the ESP32 and are skipped.
#
# Refuses a spy.bin older than any source file: a failed build leaves the
# previous binary in place.
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

BIN=spy/build/spy.bin
[ -f "$BIN" ] || { echo "no $BIN; build first (cd spy && idf.py build)"; exit 1; }
for src in spy/main/*.c spy/main/*.h spy/sdkconfig.defaults spy/partitions.csv; do
    if [ "$src" -nt "$BIN" ]; then
        echo "$BIN is older than $src; the last build did not finish. Build first."
        exit 1
    fi
done

PORT="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | grep -v usbmodem00000000000 | head -1)}"
[ -n "$PORT" ] || { echo "no serial port: is spy on USB?"; exit 1; }

. tools/board-guard.sh
board_guard spy "$PORT" "$ANY"

if [ -n "$FULL" ]; then
    ARGS="0x0 spy/build/bootloader/bootloader.bin
          0x8000 spy/build/partition_table/partition-table.bin
          0x10000 $BIN"
else
    ARGS="0x10000 $BIN"
fi

echo "flashing $BIN to $PORT${FULL:+ (full)}"
# shellcheck disable=SC2086
"$HOME/.espressif/python_env/idf5.5_py3.13_env/bin/python" \
  "$HOME/esp/esp-idf/components/esptool_py/esptool/esptool.py" \
  --chip esp32s3 --port "$PORT" -b 921600 --after hard_reset write_flash $ARGS
echo "done"
