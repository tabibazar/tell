# BLE Screen — Design

**Date:** 2026-09-05
**Status:** Approved for planning

Send arbitrary text from a Mac to the Adafruit Feather ESP32-S3 TFT over Bluetooth
Low Energy and display it on the board's built-in 1.14" screen. Each message
replaces the previous one; long text wraps.

## Context

### Hardware

Adafruit Feather ESP32-S3 TFT — VID:PID `239A:811D`, MAC `68:ee:8f:da:62:28`,
4 MB flash, 2 MB PSRAM, silicon rev v0.2. Connects over native USB CDC as
`/dev/cu.usbmodem*`; there is no USB-serial bridge chip.

Display: **ST7789, 240×135, 1.14" IPS**.

Pin assignments, taken from the `adafruit_feather_esp32s3_tft` variant in the
ESP32 Arduino core (not from memory — an earlier guess was wrong):

| Signal | GPIO |
|---|---|
| `TFT_I2C_POWER` | 21 |
| `TFT_CS` | 7 |
| `TFT_DC` | 39 |
| `TFT_RST` | 40 |
| `TFT_BACKLITE` | 45 |
| SPI SCK | 36 |
| SPI MOSI | 35 |

`TFT_I2C_POWER` must be driven high before the panel responds. This is the most
common cause of a dead screen on this board.

### Flash layout

Already on the device; this design does not change it.

| Partition | Offset | Size |
|---|---|---|
| `nvs` | `0x9000` | 20 K |
| `otadata` | `0xe000` | 8 K |
| `ota_0` | `0x10000` | 1408 K |
| `ota_1` | `0x170000` | 1408 K |
| `uf2` (factory) | `0x2d0000` | 256 K |
| `ffat` | `0x310000` | 960 K |

`otadata` is erased and `ota_1` is blank, so TinyUF2 at `0x2d0000` runs first and
chains into `ota_0`. The app therefore always boots from `ota_0`.

### Toolchain

ESP-IDF v5.5.5 at `~/esp/esp-idf`, python env at
`~/.espressif/python_env/idf5.5_py3.13_env`, esptool 4.12.0. No `arduino-cli`, no
PlatformIO. `swiftc` 6.3.3 at `/usr/bin/swiftc`. PyObjC is **not** installed on any
Python on this machine, which is why the Mac client is Swift rather than Python.

### Constraints

- **BLE only.** The ESP32-S3 radio has no Bluetooth Classic, so there is no SPP
  serial profile. macOS talks to it via CoreBluetooth over GATT.
- **The existing firmware is replaced.** It reads a QMI8658 IMU and joins WiFi, and
  no source for it exists on this machine. A byte-exact backup is kept at
  `firmware-backup/` (see Recovery).

## Architecture

Four units. Dependencies run one way, `main` → everything; the three lower units
have no edges between them, so each is testable in isolation.

```
main ──┬──> ble_uart   (NimBLE peripheral, Nordic UART Service)
       ├──> textwrap   (pure function, no hardware)
       └──> display    (SPI + esp_lcd ST7789)
```

### `display`

Owns the panel exclusively. Brings up `TFT_I2C_POWER` and `TFT_BACKLITE`,
configures SPI and the `esp_lcd` ST7789 driver, and exposes one call:

```c
void display_show_text(const char *utf8);
```

Nothing above it knows about SPI, the ST7789, or pin numbers.

Rendering goes to an off-screen framebuffer which is blitted in a single
operation. The buffer is 240 × 135 × 2 = **64,800 bytes** (~63 KB), which fits in the S3's
512 KB SRAM without using PSRAM. Drawing glyphs straight to the panel would make
partial updates visible; one blit per message removes flicker and keeps the draw
path simple.

The panel needs `set_gap(40, 53)` — a 240×135 ST7789 sits inside the
controller's 240×320 address space. With `swap_xy(true)` the 240px axis maps to
the controller's 320-long axis (offset 40) and the 135px axis to the 240-long
axis (offset 53). **Verified on hardware 2026-09-06**; the reversed `(53, 40)`
clips roughly 13px off the top row.

### `textwrap`

Pure, hardware-free, and therefore the only unit worth testing thoroughly:

```c
size_t textwrap(const char *utf8, size_t cols, size_t max_lines,
                char lines[][COLS_MAX + 1]);
```

Greedy word wrap at `cols` columns. A word longer than `cols` is broken
mid-word. Returns the number of lines produced.

### `ble_uart`

NimBLE peripheral. Advertises, exposes the Nordic UART RX characteristic, and
invokes a callback per received message. Knows nothing about displays or text
layout.

### `main`

Wires `ble_uart`'s callback through `textwrap` into `display`.

## BLE protocol

Nordic UART Service, standard UUIDs, chosen so that any generic BLE app can drive
the board before the Swift client exists:

| Item | Value |
|---|---|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX characteristic | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |
| Properties | Write, Write Without Response |
| Advertised name | `ESP32-Screen` |

No pairing or bonding. The payload is text on a desk device; pairing would add a
Mac-side flow without buying meaningful security.

One write is one message, replacing whatever is on screen. Messages are capped at
**512 bytes** (the ATT maximum). The client truncates longer input and warns
rather than silently cutting.

### MTU and chunking

Default BLE MTU is 23 bytes — **20 bytes of payload per write**. macOS negotiates
a larger MTU automatically on connect, and with MTU 517 a full 512-byte message
fits in one write.

If a message exceeds the negotiated MTU, the client splits it and the firmware
reassembles: appends chunks to a buffer, and treats the message as complete when
either the buffer is full or **50 ms** pass with no further write. This is the
single most likely source of "long messages arrive truncated", so it gets an
explicit test.

## Display behaviour

- 12×24 monospace bitmap font compiled into the binary. This divides the panel
  exactly: **20 columns × 5 rows**, no partial cells. (Revised from 8×16 / 30×8
  on 2026-09-05 after testing on hardware: 8×16 was too small to read.)
- Text wraps at 20 columns.
- If wrapping yields more than 5 lines, the first 5 are shown and the final cell
  is replaced with `...` (three ASCII dots; the font is ASCII-only). Truncation is always visible — silently dropping text
  would make the display untrustworthy.
- White on black. Not configurable; add it when it is actually wanted.

## Mac client

A single Swift file compiled with the installed `/usr/bin/swiftc` into a
standalone `esp32-say` binary. No runtime dependencies.

```
esp32-say "some text"     # argument
echo "some text" | esp32-say   # stdin
```

Uses CoreBluetooth: scan for the service UUID, connect, discover the RX
characteristic, write, disconnect. Exits non-zero with a message on stderr if the
device is not found within a **10 second** timeout.

Scanning by service UUID rather than by name matters on macOS, because
CoreBluetooth caches peripheral names aggressively and a renamed device can
otherwise be missed.

## Error handling

| Condition | Behaviour |
|---|---|
| Panel init fails | Log over serial and halt. There is no way to report a display fault on the display. |
| BLE stack fails to start | Log over serial, show `BLE FAILED` on the panel. |
| Message longer than 512 bytes | Client truncates and warns on stderr. |
| Message wraps past 8 lines | Firmware shows the first 8 lines, ends with `...`. |
| Empty message | Clears the screen. Explicitly allowed; it is how you blank the display. |
| Invalid UTF-8 | Non-renderable bytes become `?`. Never drop the whole message. |
| Device not found | Client exits non-zero after 10 s with a message on stderr. |

## Testing

Built in dependency order, so each step rests on something already trusted.

1. **`textwrap`** — host unit tests compiled with `cc` and run on the Mac, no
   board involved. Cases: exact-width lines, over-long words, empty input,
   multi-line overflow with the `...` marker, UTF-8 input. TDD applies here.
2. **`display`** — flash with a hardcoded string. Confirms pins, the (40, 53)
   gap, orientation, and colours before any BLE code exists.
3. **`ble_uart`** — drive from LightBlue or any generic BLE app, so a failure is
   unambiguously firmware-side rather than Swift-side.
4. **End-to-end** — `esp32-say`, with the serial watcher running for the
   firmware's own view. Includes a message long enough to force MTU chunking.

## Build and flash

Target `esp32s3`, ESP-IDF 5.5.5. Estimated 700–900 KB with NimBLE and `esp_lcd`,
inside `ota_0`'s 1408 KB.

**Flash the app only:**

```
esptool.py --port /dev/cu.usbmodem* write_flash 0x10000 build/screen.bin
```

Do not write the bootloader, the partition table, or `otadata`. Preserving them
keeps TinyUF2 as the recovery path and leaves the boot chain untouched. A
partition CSV matching the on-flash layout is generated so the app's runtime view
agrees with reality, but it is never flashed.

Do not pass `--baud 921600`: USB-Serial-JTAG ignores it and the port drops with
`Device not configured`.

### Recovery

Both routes were exercised during this session:

- `write_flash 0x10000 firmware-backup/stock-tabriz.bin` restores the firmware
  that was running before the replacement; `stock-<network-a>.bin` is the untouched
  original. See `firmware-backup/README.md`.
- Double-tap RESET mounts the TinyUF2 drive, for the case where a build does not
  boot at all.

### Known risk

The on-flash bootloader was built by whoever produced the current firmware, not by
IDF 5.5.5. IDF app images are normally forward-compatible with older bootloaders.
If the app refuses to boot, this is the cause, and the fix is flashing a matching
bootloader to `0x0` — a deliberate step to take with the UF2 recovery path
available, not a preemptive one.

## Out of scope

- Re-implementing the QMI8658 IMU readout or WiFi from the old firmware.
- Scrollback, message history, or a log view. One message at a time.
- Colour, font, or brightness control.
- Pairing, bonding, or encryption.
