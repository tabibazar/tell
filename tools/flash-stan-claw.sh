#!/bin/sh
# Flash stan-claw onto the Waveshare ESP32-S3-Touch-LCD-4B (panel1's board).
#
#   tools/flash-stan-claw.sh --full    bootloader + partition table + app (first time)
#   tools/flash-stan-claw.sh           app only
#
# Back up panel1's settings first (secrets/panel1-nvs.bin); the NVS partition
# is left where it is, and stan-claw imports panel1's WiFi from it.
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
BIN=stan-claw/build/stan-claw.bin
[ -f "$BIN" ] || { echo "no $BIN; build first (cd stan-claw && idf.py build)"; exit 1; }
for src in stan-claw/main/*.c stan-claw/main/*.h stan-claw/sdkconfig.defaults; do
    if [ "$src" -nt "$BIN" ]; then
        echo "$BIN is older than $src; the last build did not finish. Build first."
        exit 1
    fi
done
PORT="${1:-$(ls /dev/cu.usbmodem5B91* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "no port: is the board on USB?"; exit 1; }
. tools/board-guard.sh
board_guard panel1 "$PORT" "$ANY"
if [ -n "$FULL" ]; then
    ARGS="0x0 stan-claw/build/bootloader/bootloader.bin
          0x8000 stan-claw/build/partition_table/partition-table.bin
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
