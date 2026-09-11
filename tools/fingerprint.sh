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
#
# Each read resets the board into the bootloader itself rather than assuming an
# earlier command left it there. It used to pass --before no_reset, which works
# only if the previous step happened to leave the board in the downloader; when
# it did not, the read failed with "No serial data received", the error went to
# /dev/null, and the section came out empty rather than saying anything. An
# empty partition table in a fingerprint is worse than no fingerprint, because
# it reads as "this board has none".
TMP=$(mktemp -d "${TMPDIR:-/tmp}/fingerprint.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

# Reads a flash region into $TMP/$3. Prints why it failed and returns non-zero.
esp_read() {
    if "$PY" "$ESPTOOL" --port "$PORT" --after no_reset \
            read_flash "$1" "$2" "$TMP/$3" >"$TMP/err" 2>&1; then
        return 0
    fi
    echo "(could not read $2 bytes at $1)"
    grep -iE 'fatal|error' "$TMP/err" | head -2 | sed 's/^/  /'
    return 1
}

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
# grep -Ev with | rather than BRE \|, which BSD grep on macOS takes literally.
if esp_read 0x8000 0xc00 ptable.bin; then
    if ! "$PY" "$GENPART" "$TMP/ptable.bin" 2>"$TMP/gperr" \
         | grep -Ev '^#|^Parsing|^Verifying'; then
        echo "(no readable partition table at 0x8000)"
        grep -iE 'error|invalid' "$TMP/gperr" | head -2 | sed 's/^/  /'
    fi
fi
echo '```'
echo

echo "## Running application"
echo
echo '```'
# esp_app_desc_t sits at 0x20: after the 24-byte image header and the first
# 8-byte segment header. It is 256 bytes long.
if esp_read 0x10000 0x130 app.bin; then
    "$PY" tools/_appdesc.py "$TMP/app.bin"
fi
echo '```'

# Leave the board running its application again: every read above finished with
# --after no_reset, so without this it would sit in the bootloader afterwards.
#
# esptool's own --after hard_reset is not enough on these boards. It prints
# "Hard resetting via RTS pin" and the board stays in the downloader, which is
# the same native USB-Serial-JTAG quirk that makes DTR/RTS auto-reset useless
# when flashing. Driving the lines directly does work, and the polarity is the
# whole trick: DTR is IO0, so it must stay de-asserted or the chip comes up in
# the ROM downloader instead of the app, and RTS is EN, so pulsing it alone is
# what reboots it.
"$PY" - "$PORT" >/dev/null 2>&1 <<'RESET' || true
import sys, time, serial
p = serial.Serial(sys.argv[1], 115200, timeout=0.2)
p.setDTR(False)
p.setRTS(True); time.sleep(0.15)
p.setRTS(False)
p.close()
RESET
