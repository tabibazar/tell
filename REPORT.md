# Overnight report — 2026-09-05

## What the board runs now

Your own firmware, built from this repo: a BLE peripheral that displays text you
send from the Mac. The stock QMI8658 + WiFi firmware is **replaced, not lost** —
see `firmware-backup/`.

## Using it

```sh
mac/esp32-say "hello"              # argument
echo "hello" | mac/esp32-say       # stdin
mac/esp32-say ""                   # clears the screen
```

If `mac/esp32-say` is missing, rebuild it with `./mac/build.sh`.

## What I could not verify — please check this first

**I never saw the screen.** Serial proves `display_init()` returned `ESP_OK`, which
means SPI and the ST7789 *accepted* their configuration — not that the image is
oriented or positioned correctly. You reported the first version was upside down;
I flipped it, but I am trusting the fix rather than observing it.

The message on screen right now is an orientation test:

```
^ TOP font 12x24
BLE + display OK
all tests passed
see REPORT.md
v BOTTOM  -Claude
```

- **`^ TOP` on top** → orientation is correct, nothing to do.
- **`v BOTTOM` on top** → still 180° out. In `main/display.c`, change
  `esp_lcd_panel_mirror(s_panel, true, false)` back to `(false, true)`.
- **Mirrored / readable in a mirror** → flip only one axis: `(true, true)` or
  `(false, false)`.
- **Shifted or wrapped by a few dozen pixels** → the offset, not the rotation:
  `esp_lcd_panel_set_gap(s_panel, 53, 40)`, try `(40, 53)` or `(52, 40)`.

Rebuild and flash after any change:

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
marker on the panel. I verified those at the wrap layer with unit tests and
confirmed the bytes reach the firmware, but the on-screen result is unconfirmed
for the same reason as orientation: I cannot see the display.
