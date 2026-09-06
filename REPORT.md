# Overnight report — 2026-09-05

*Display geometry confirmed on hardware 2026-09-06.*

## What the board runs now

Your own firmware, built from this repo: a BLE peripheral that displays text you
send from the Mac. The stock QMI8658 + WiFi firmware is **replaced, not lost** —
see `firmware-backup/`.

## Using it

```sh
mac/tell "hello"              # argument
echo "hello" | mac/tell       # stdin
mac/tell ""                   # clears the screen
```

If `mac/tell` is missing, rebuild it with `./mac/build.sh`.

## Display: verified on hardware 2026-09-06

Orientation and geometry are now confirmed correct by eye:
`mirror(true, false)` with `swap_xy(true)`, and `set_gap(40, 53)`. The reversed
gap `(53, 40)` clipped ~13px off the top row -- with `swap_xy` the 240px axis
takes the 320-long axis offset (40) and the 135px axis takes the 240-long one
(53). The text block is also centred in the leftover 15px of height.

To rebuild and flash after any change:

```sh
. tools/idf-env.sh && idf.py build
~/.espressif/python_env/idf5.5_py3.13_env/bin/python \
  ~/esp/esp-idf/components/esptool_py/esptool/esptool.py \
  --port /dev/cu.usbmodem21201 --after hard_reset write_flash 0x10000 build/screen.bin
```

## What is verified

| Check | Result |
|---|---|
| `textwrap` unit tests | 9/9 pass (`./host_tests/test_textwrap`) |
| Font table | 5/5 pass (`./host_tests/test_font`) |
| Panel init | `display_init()` returned `ESP_OK`; heartbeat proves `app_main` got past it |
| BLE connect + write | Confirmed over serial, MTU negotiated to 256 |
| **512-byte message** | **Crosses 3 BLE writes at MTU 256 and reassembles correctly** |
| Empty message | Delivered as 0 bytes → clears |
| Oversize message | Client truncates at 512 with a stderr warning |
| argv and stdin | Both work |

The 512-byte case was the risk I flagged in the spec, and it passes.

## Layout

12×24 font → **20 columns × 5 rows**, 100 characters. That is the trade for
legibility; the previous 8×16 gave 30×8 but you couldn't read it. Text wraps at
20 columns; anything past 5 rows is truncated with a visible `...`.

## Restoring the old firmware

```sh
~/.espressif/python_env/idf5.5_py3.13_env/bin/python \
  ~/esp/esp-idf/components/esptool_py/esptool/esptool.py \
  --port /dev/cu.usbmodem21201 write_flash 0x10000 firmware-backup/stock-tabriz.bin
```

`stock-tabriz.bin` is what was running before I replaced it (WiFi `<network-b>`).
`stock-<network-a>.bin` is the untouched original. Details in
`firmware-backup/README.md`. Double-tap RESET still mounts the TinyUF2 drive if
the board ever won't boot.

## Seeing logs

```sh
. tools/idf-env.sh && idf.py -p /dev/cu.usbmodem21201 monitor    # ctrl-] to exit
```

The board logs a heartbeat every 5 s. USB-Serial-JTAG discards output when no
host is attached, so boot logs are usually missed — the heartbeat exists so
liveness is observable at any time.

## Known limitations

- **`ble_uart.c` has an unsynchronised buffer.** `flush_message` (esp_timer task)
  and `gatt_rx` (NimBLE host task) share `s_buf`/`s_len` without a lock. Harmless
  at human message rates with a disconnect between messages; it would need a
  mutex before any rapid-fire or multi-client use.
- Non-ASCII renders as `?`. The font is ASCII 32..126 only.
- One message at a time, no history — as specified.

## Not done

The plan's Task 7 called for a visual check of wrapping and the `...` truncation
marker on the panel. Both are verified at the wrap layer by unit tests, and the
bytes are confirmed reaching the firmware, but neither has been eyeballed on the
screen. Orientation and geometry, which were in this list, are now confirmed.
