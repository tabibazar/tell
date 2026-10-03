#!/bin/sh
# Flash tiny1 (Adafruit Feather ESP32-S3 TFT clone) with the app in tiny1/build/.
#
#   tools/flash-tiny1.sh            app only, finds the CP2102 port
#   tools/flash-tiny1.sh --full     bootloader + partition table + app
#
# tiny1 has only the S3's own USB. With our firmware it is USB-Serial-JTAG,
# which esptool resets into the bootloader by itself (the factory Arduino
# firmware's TinyUSB port could not be: that took BOOT + RESET by hand).
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

BIN=tiny1/build/tiny1.bin
[ -f "$BIN" ] || { echo "no $BIN; build first (cd tiny1 && idf.py build)"; exit 1; }
for src in tiny1/main/*.c tiny1/sdkconfig.defaults; do
    if [ "$src" -nt "$BIN" ]; then
        echo "$BIN is older than $src; the last build did not finish. Build first."
        exit 1
    fi
done

PORT="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | grep -v -e usbmodem00000000000 -e 5B91 | head -1)}"
[ -n "$PORT" ] || { echo "no port: is tiny1 on USB?"; exit 1; }

. tools/board-guard.sh
board_guard tiny1 "$PORT" "$ANY"

if [ -n "$FULL" ]; then
    ARGS="0x0 tiny1/build/bootloader/bootloader.bin
          0x8000 tiny1/build/partition_table/partition-table.bin
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
