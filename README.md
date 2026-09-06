# tell

Push text from your Mac to an ESP32 desk display over Bluetooth LE.

```sh
tell "build passed"
date | tell
git log -1 --format=%s | tell
```

When nothing is being displayed, the screen shows a clock.

---

## What this is

Firmware for an **Adafruit Feather ESP32-S3 TFT** that advertises a BLE service
and renders whatever text you write to it on the board's 1.14" screen, plus a
small Swift command-line client for macOS.

No pairing, no WiFi, no network configuration, no app. The board can run off a
battery anywhere in BLE range; the Mac finds it and writes to it in about a
second.

**Why BLE and not WiFi or USB serial?** WiFi means credentials baked into
firmware and a board that stops working when the network changes. USB serial
means a cable. BLE means the thing sits wherever you like and any Mac in range
can write to it. (Classic Bluetooth serial — SPP — is not an option: the
ESP32-S3 radio does not implement it.)

## Hardware

| | |
|---|---|
| Board | Adafruit Feather ESP32-S3 TFT |
| Display | ST7789, 240×135, 1.14" IPS |
| Connection | Native USB (no USB-serial bridge chip) |

No wiring or soldering — the display is part of the board.

## Install the client

Grab `tell.tar.gz` from [Releases](../../releases), then:

```sh
tar xzf tell.tar.gz && cd tell
xattr -dr com.apple.quarantine tell     # only if it arrived via download or AirDrop
sudo mv tell /usr/local/bin/
```

The binary is universal (Apple Silicon and Intel), about 90 KB, and links only
against system frameworks and the Swift runtime that ships with macOS 11+. There
is nothing else to install.

macOS asks for Bluetooth permission on first run. The prompt comes from your
**terminal application**, not from `tell`.

Or build it yourself — `swiftc` comes with the Xcode command line tools:

```sh
./mac/build.sh
```

## Usage

```sh
tell "hello"                  # display text
echo "hello" | tell           # from stdin, so anything can pipe into it
date | tell
tell ""                       # clear, returning to the clock
tell --sync                   # sync the clock without changing the display
```

The screen is **20 characters by 5 lines**. Text wraps at word boundaries; a
word longer than a line is broken mid-word. Content past five lines is truncated
with a visible `...`, so text is never silently dropped. `\n` forces a break.
Bytes outside printable ASCII render as `?`.

A message holds the screen for 30 seconds, then the clock returns.

## The clock

The board has no battery-backed real-time clock, so it starts at `--:--:--`
after every power cycle and drifts a few seconds a day. Every `tell` command
re-syncs it, so in practice this is invisible; `tell --sync` corrects it without
disturbing what is on screen.

Time is sent as **seconds since your local midnight**, not a Unix timestamp.
That way the firmware never needs to know about timezones or leap seconds — it
counts seconds and formats `HH:MM:SS`.

## How it works

```
Mac (CoreBluetooth) ──BLE GATT──> ESP32-S3 (NimBLE) ──SPI──> ST7789
```

The firmware is four units with one-way dependencies, so each can be tested
alone:

| Unit | Responsibility |
|---|---|
| `ble_uart` | NimBLE peripheral, GATT server, message reassembly |
| `textwrap` | Word wrap. Pure C, no hardware — this is where the unit tests live |
| `display`  | SPI, ST7789, framebuffer, glyph rendering |
| `timecalc` | Seconds-since-midnight arithmetic. Also pure, also tested |

`main` wires them together. Nothing above `display` knows about SPI or pin
numbers, and nothing above `ble_uart` knows about GATT.

Rendering goes to an off-screen framebuffer that is blitted in one operation.
Drawing glyphs straight to the panel would make partial updates visible.

### BLE protocol

The board advertises as `ESP32-Screen` with the **Nordic UART Service**, so any
generic BLE app (LightBlue, nRF Connect) can drive it — useful for telling
firmware bugs apart from client bugs.

| | UUID | |
|---|---|---|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | |
| Text | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | write, UTF-8, max 512 bytes |
| Clock | `6E400004-B5A3-F393-E0A9-E50E24DCCA9E` | write, 4 bytes LE, seconds since local midnight |

No pairing or bonding: it displays text on a desk, and pairing would add a setup
step without buying meaningful security.

**Message framing.** BLE's default MTU is 23 bytes — 20 bytes of payload per
write. macOS negotiates upward on connect, but a long message still arrives as
several writes. The firmware appends them to a buffer and completes the message
after **50 ms** with no further write. This is the part most likely to break, so
it has an explicit test: a 512-byte message crosses three writes at MTU 256 and
reassembles correctly.

The 128-bit service UUID plus flags fills a 31-byte advertisement, so the device
name is sent in the scan response instead.

### The font

Generated from Menlo by `tools/gen_font.py` into a C header, so there is no font
library on the device and no runtime rasterisation. A 12×24 cell divides the
240×135 panel exactly into 20×5 with no partial cells.

The clock reuses the same table, scaled up by pixel replication to the largest
integer factor that fits.

## Building the firmware

Needs [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/)
5.5 or later.

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py build
```

Flash **only the application**:

```sh
esptool.py --port /dev/cu.usbmodem* write_flash 0x10000 build/screen.bin
```

> **Flash the app and nothing else.** `idf.py flash` would also write the
> bootloader, partition table and OTA data. On an Adafruit board that overwrites
> TinyUF2 — the double-tap-RESET recovery drive — and you lose your safety net.
> `idf.py` also *suggests* `0x2d0000` for the app, because the partition table
> labels the TinyUF2 slot `factory`. Ignore that; the app belongs at `0x10000`.

Two more traps on this board, both learned the hard way:

- **Do not pass `--baud 921600`.** USB-Serial-JTAG ignores the baud setting and
  the port drops with `Device not configured`. Use the default rate.
- **After a failed build, `idf.py` leaves the previous `build/screen.bin` in
  place.** Check the binary's timestamp before flashing, or you will flash a
  stale image and debug a bug you already fixed.

Serial output goes over USB-Serial-JTAG (`idf.py monitor`). It is discarded when
no host is attached, so boot logs are usually missed — hence the heartbeat log
every 30 seconds.

## Tests

The two pure units are tested on the host, no board required:

```sh
make -C host_tests && for t in host_tests/test_*; do [ -x "$t" ] && "$t"; done
```

Covers wrap boundaries, over-long words, truncation marking, non-ASCII
substitution, and — for the clock — midnight wrap and multi-day rollover.

## Known limitations

- `ble_uart.c` shares its message buffer between the esp_timer callback and the
  NimBLE host task without a lock. Harmless at human message rates with a
  disconnect between messages; it would need a mutex for rapid-fire or
  multi-client use.
- ASCII 32–126 only.
- One message at a time; no history or scrollback.
- One Mac connected at a time — though each message holds the connection for
  only about a second.

## Repository layout

```
main/           firmware (ESP-IDF component)
mac/            tell.swift, the macOS client
host_tests/     unit tests for the pure C units
tools/          font generator, dist packaging, IDF env helper
docs/           design spec and implementation plan
```

## License

MIT. See [LICENSE](LICENSE).
