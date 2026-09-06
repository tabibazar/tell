#!/bin/sh
# Captures an identifying profile of an attached ESP32 board.
#
#   tools/fingerprint.sh [port] > docs/hardware/<board>.md
#
# Read-only: it reads chip identity, eFuses, flash and the partition table, and
# never writes to the device. The board is reset back into its application on
# exit.
set -e
cd "$(dirname "$0")/.."

PORT="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "no /dev/cu.usbmodem* found" >&2; exit 1; }

PY="$HOME/.espressif/python_env/idf5.5_py3.13_env/bin/python"
ESPTOOL="$HOME/esp/esp-idf/components/esptool_py/esptool/esptool.py"
ESPEFUSE="$HOME/esp/esp-idf/components/esptool_py/esptool/espefuse.py"
GENPART="$HOME/esp/esp-idf/components/partition_table/gen_esp32part.py"

# Never raise the baud rate: USB-Serial-JTAG ignores it and the port drops.
esp() { "$PY" "$ESPTOOL" --port "$PORT" --before no_reset --after no_reset "$@" 2>&1; }

echo "# Board fingerprint"
echo
echo "Captured $(date '+%Y-%m-%d %H:%M %Z') from \`$PORT\`."
echo

# The board's USB serial number is its MAC, so read the MAC first and use it to
# pick this device out of the USB tree rather than dumping every peripheral.
MAC=$("$PY" "$ESPTOOL" --port "$PORT" --after no_reset read_mac 2>&1 \
      | sed -n 's/^MAC: *//p' | head -1)

echo "## USB identity"
echo
echo '```'
if [ -n "$MAC" ]; then
  ioreg -p IOUSB -l -w 0 2>/dev/null \
    | grep -iB 30 -A 10 "\"USB Serial Number\" = \"$MAC\"" \
    | grep -E '"(USB Product Name|USB Vendor Name|idVendor|idProduct|USB Serial Number)"' \
    | sed 's/^[^"]*//' | sort -u
else
  echo "(MAC unavailable; cannot identify the USB node)"
fi
echo '```'
echo

echo "## Chip"
echo
echo '```'
"$PY" "$ESPTOOL" --port "$PORT" --after no_reset flash_id 2>&1 \
  | grep -viE '^esptool|^Serial port|^Connecting|Uploading stub|Running stub|Stub running|Configuring flash|Leaving|Staying in|Hard resetting'
echo '```'
echo

echo "## eFuses"
echo
echo '```'
"$PY" "$ESPEFUSE" --port "$PORT" --do-not-confirm summary 2>&1 \
  | grep -E 'MAC|WAFER|PKG_VERSION|FLASH|PSRAM|BLOCK_VERSION|SECURE_BOOT_EN|SPI_BOOT_CRYPT|DIS_DOWNLOAD|USB_' \
  | sed 's/  */ /g' | head -30
echo '```'
echo

echo "## Partition table"
echo
echo '```'
esp read_flash 0x8000 0xc00 /tmp/_fp_ptable.bin >/dev/null 2>&1 || true
"$PY" "$GENPART" /tmp/_fp_ptable.bin 2>/dev/null | grep -v '^#' | grep -v '^Parsing\|^Verifying' || echo "(unreadable)"
rm -f /tmp/_fp_ptable.bin
echo '```'
echo

echo "## Running application"
echo
echo '```'
# esp_app_desc_t sits at 0x20: after the 24-byte image header and the first
# 8-byte segment header. It is 256 bytes long.
esp read_flash 0x10000 0x130 /tmp/_fp_app.bin >/dev/null 2>&1 || true
if [ -f /tmp/_fp_app.bin ]; then
  "$PY" tools/_appdesc.py /tmp/_fp_app.bin
  rm -f /tmp/_fp_app.bin
fi
echo '```'

# Leave the board running its application again. Without --before no_reset the
# stub is re-uploaded, which is what makes the reset actually stick.
"$PY" "$ESPTOOL" --port "$PORT" --after hard_reset flash_id >/dev/null 2>&1 || true
