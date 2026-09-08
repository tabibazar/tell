# tell

Push text from your Mac to an ESP32 desk display over Bluetooth LE.

```sh
tell "build passed"
date | tell
git log -1 --format=%s | tell
```

When nothing is being displayed, the screen shows a clock. On the big panel
there are also pages of Claude Code usage: bars per model, bars per day, a
line per model over the last two months, a heatmap of the last year in the
style of Claude Code's own `/stats` screen, a weekday-by-hour heatmap of when
you work, a live page for today with whether Claude is busy right now, the
tokens by repository, the prompt-cache hit rate and what caching saved, which
tools Claude calls and the programs behind its Bash calls, how much of its
output is thinking, this week against
last, your personal records, what it all would have cost at API list
prices, and a recap of the day written by Claude from your prompts, plus an
almanac and a Settings page. While Claude is busy the board shows the live page on its own and
returns to the clock when the work stops; Settings can turn that off.

On the touch panel a tap on the right half of the screen goes to the next
page and one on the left half to the previous; the amber MENU tab at the top
left of every page opens a menu with a tile per page, so any page is two taps
away. After ten minutes with no tap
and no message the screensaver cycles through the pages, twenty seconds each,
so nothing sits still long enough to burn in; a tap brings back whatever page
is showing. The Settings page (last in the
tap order, touch boards only) changes the delay, from one minute to never,
switches the saver to a drifting clock instead, sets the seconds per page,
chooses whether the board jumps to the live page while Claude is busy, and
picks the home page: the clock, or the live page, which then stays up all day
if the screensaver is set to never. Settings are kept in flash across power
cycles.

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

| Board | Display | Layout | Status |
|---|---|---|---|
| Adafruit Feather ESP32-S3 TFT | ST7789 240×135, SPI | 20 × 5 | Verified |
| Elecrow CrowPanel 7.0" HMI | 800×480, 16-bit RGB | 64 × 20 | Verified |

No wiring or soldering — the display is part of the board. Select the target
with `idf.py menuconfig` under *Screen board*; see [docs/porting.md](docs/porting.md)
for the CrowPanel build and its many inverted gotchas.

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
tell --device big "hello"     # pick a board by name when several are in range
```

With more than one board powered up, `tell` connects to whichever answers
first. Give each board its own name (`CONFIG_SCREEN_DEVICE_NAME` in
menuconfig) and address it with `--device`.

## Setting up a Mac

Nothing is paired, so any Mac in range can drive the boards:

```sh
git clone git@github.com:tabibazar/tell.git
cd tell
./install.sh
```

That checks the prerequisites, builds the client, links it onto your PATH,
looks for the boards, and starts the agents that keep the clock, charts,
almanac and story fresh. `./install.sh big --no-agents` sets up the client alone.

macOS asks your **terminal** for Bluetooth permission the first time; if no
board is found, grant it under System Settings > Privacy & Security >
Bluetooth and run the script again.

Each machine tags its data with its own name, and the board keeps them apart:
a push from one Mac replaces only its own share, and every bar is drawn
stacked with a segment per machine, named in the legend. Set
`CLAUDE_SCREEN_HOST` to choose the label.

Only a Mac that can see the board over Bluetooth can contribute, so this means
machines in the same room. A machine that stops pushing keeps whatever it last
sent until the board restarts.

The usage pages are pushed every five minutes, and the live page's section
on its own every minute so "busy" stays current; the clock, almanac and story
agents run on their own timers.

The board accepts one BLE connection at a time. Sends on the same machine take
a shared lock; across machines the client simply retries, five times, ten
seconds apart.

The screen is **20 characters by 5 lines** on the Feather, **64 by 20** on the
CrowPanel. Text wraps at word boundaries; a
word longer than a line is broken mid-word. Content past five lines is truncated
with a visible `...`, so text is never silently dropped. `\n` forces a break.
Bytes outside printable ASCII render as `?`.

A message holds the screen for 5 minutes, then the clock returns.

## The clock

The board has no real-time clock of its own, so on its own it starts at
`--:--:--` after every power cycle and drifts a few seconds a day. Every `tell`
command re-syncs it, so in practice this is invisible; `tell --sync` corrects
it without disturbing what is on screen.

A **DS3231 module on the CrowPanel's I2C header** fixes the power-cycle gap:
the firmware finds it at boot, takes the time from it, and writes every sync
from a Mac back to it, so the chip always holds the last NTP-disciplined time
the Mac had. Once an hour without a sync it re-reads the chip to cancel the
ESP timer's drift. The chip stores local time of day only; after a
daylight-saving change it is an hour off until the next sync, which the
`push-clock` agent provides within five minutes. A chip whose battery has
died reports that its oscillator stopped, and is then ignored until set again.

Time is sent as **seconds since your local midnight**, not a Unix timestamp.
That way the firmware never needs to know about timezones or leap seconds — it
counts seconds and formats `HH:MM:SS`. The one concession is the title bar on
the big panel's pages, which shows the local time with its zone and UTC beside
it; the offset and the zone's name (`utc -240`, `tz EDT`) ride along in the
clock payload the `push-clock` agent sends every five minutes.

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
| `canvas`   | Framebuffer and glyph rendering. Panel independent, host tested |
| `display_*`| Panel bring-up and blitting. One file per board |
| `timecalc` | Seconds-since-midnight and days-since-epoch arithmetic. Also pure, also tested |
| `usagedata` + `ud_*` | Parses and merges the data payloads from several Macs: a small core and one file per payload, registered in `ud_sections.c`. Pure, tested |
| `view_*`   | Draws the pages onto a canvas, one file per page over `view_common`. Rendered and inspected on the host |
| `pagedefs` | The page table: name, which payload feeds it, how it is drawn. `main` reads it instead of listing pages |

`main` wires them together. Nothing above `display` knows about SPI or pin
numbers, and nothing above `ble_uart` knows about GATT. Adding a page is a
`view_*.c` file, a `ud_*.c` file if it needs a new payload, an enum entry,
and one row in each of the two tables; see [docs/developing.md](docs/developing.md).

Rendering goes to an off-screen framebuffer that is blitted in one operation.
Drawing glyphs straight to the panel would make partial updates visible.

### BLE protocol

The board advertises as `ESP32-Screen` with the **Nordic UART Service**, so any
generic BLE app (LightBlue, nRF Connect) can drive it — useful for telling
firmware bugs apart from client bugs.

| | UUID | |
|---|---|---|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | |
| Text | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | write, UTF-8, max 2048 bytes |
| Clock | `6E400004-B5A3-F393-E0A9-E50E24DCCA9E` | write, 4 bytes LE, seconds since local midnight |

**Data payloads.** A text message that starts with `!stats`, `!daily`,
`!year`, `!cost`, `!rhythm`, `!now`, `!projects`, `!cache`, `!tools`,
`!thinking`, `!week`, `!records`, `!runs`, `!turns`, `!story`, `!clock` or
`!today` is data for a page rather than a
message to show, and replaces that section for the sending machine (named on
a `host` line) without changing what is on screen. `tools/claude-stats.py
--format data --section <name>` produces each of the first fourteen (or `--all
DIR` writes them all from one pass); `tools/story.py` writes the recap by
handing the day's prompts to the `claude` command in headless mode, on your
subscription, and regenerates only when the prompt count has changed; the
almanac and weather scripts produce the last two.

The script reads the transcripts under `~/.claude/projects` for recent, exact
figures and merges `~/.claude/stats-cache.json` for the months before that,
since transcripts are pruned but the cache keeps a daily summary back to the
first session. Days the cache knows only as message counts are estimated from
the average message and footnoted on the board. Costs use API list prices
per model, cache reads at the model's read rate, cache writes at 1.25x input
for the 5-minute TTL or 2x for the 1-hour TTL when the transcript says which;
set `CLAUDE_PLAN_USD` to your subscription price to see the ratio.

Dates in these payloads are **day numbers**, days since 1970-01-01 in the
Mac's local time, and per-day token counts are **one character per day**:
`.` for none, otherwise `0-9A-Za-z` on a half-octave log scale
(`round(2*log2(tokens/1000))`, decoded as `1000*2^(i/2)`, within about 19%).
That keeps a year of days to 371 bytes and eight models' last sixty days to
about a kilobyte, so each fits one 2048-byte message however heavy the usage.
The board turns day numbers back into month names and weekdays itself
(`timecalc_civil`); it still knows nothing about time zones.

No pairing or bonding: it displays text on a desk, and pairing would add a setup
step without buying meaningful security.

**Message framing.** BLE's default MTU is 23 bytes — 20 bytes of payload per
write. macOS negotiates upward on connect, but a long message still arrives as
several writes. The firmware appends them to a buffer and completes the message
after **250 ms** with no further write, or **1.5 s** for a data payload (one
starting with `!`), since a stall in the Mac's Bluetooth stack mid-payload
otherwise closes it early and the tail shows up as a text message with no
marker. This is the part most likely to break, so
it has an explicit test: a 512-byte message crosses three writes at MTU 256 and
reassembles correctly.

The 128-bit service UUID plus flags fills a 31-byte advertisement, so the device
name is sent in the scan response instead.

### The font

Generated from Menlo by `tools/gen_font.py` into a C header, so there is no font
library on the device and no runtime rasterisation. A 12×24 cell divides the
240×135 panel exactly into 20×5, and gives 64×20 on the 800×480 panel.

The clock is magnified the most, and on a large panel pixel replication made it
visibly blocky. So the CrowPanel carries a second table rendered at its final
96×160 size. That is affordable because a clock needs only `0`–`9` and `:`,
which are contiguous in ASCII — 11 glyphs at 21 KB, where a full alphabet at
that size would be 182 KB. Smaller panels fall back to magnifying the body
font, which is fine at their scale.

## Building the firmware

Needs [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/)
5.5 or later. **[docs/developing.md](docs/developing.md)** covers setting up a
new machine, and the traps on this hardware that are worth reading before you
hit them.

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
  stale image and debug a bug you already fixed. `tools/flash-crowpanel.sh`
  refuses a binary older than the sources for exactly this reason, then
  flashes the app partition and pushes every data section.

Serial output goes over USB-Serial-JTAG (`idf.py monitor`). It is discarded when
no host is attached, so boot logs are usually missed — hence the heartbeat log
every 30 seconds.

## Tests

The pure units are tested on the host, no board required:

```sh
make -C host_tests && for t in host_tests/test_*; do [ -x "$t" ] && "$t"; done
```

Covers wrap boundaries, over-long words, truncation marking, non-ASCII
substitution, the clock's midnight wrap and multi-day rollover, the calendar
arithmetic, the payload parser and multi-machine merge, and the page layouts.
`host_tests/test_views /tmp/page tools-output.txt ...` also writes each usage
page as a PPM, so a layout can be looked at without flashing anything:
`sips -s format png /tmp/page-year.ppm --out page-year.png`.

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
