# Developing on this project

What a new machine needs, and the traps that cost time on this hardware.

## Setting up a machine

The Mac client needs only the Xcode command line tools:

```sh
./install.sh            # builds tell, links it, starts the agents
```

Firmware work additionally needs **ESP-IDF 5.5 or later**, which is a few GB:

```sh
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.5 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
```

**Do not source `export.sh` directly.** It derives its virtualenv name from
whatever `python3` currently is, so a Python upgrade leaves it looking for an
environment that does not exist:

```
ERROR: ESP-IDF Python virtual environment ".../idf5.5_py3.14_env" not found.
```

Source `tools/idf-env.sh` instead, which pins `IDF_PYTHON_ENV_PATH` to the
environment that actually exists. Edit that file if your version differs.

## Building and flashing

```sh
. tools/idf-env.sh

# Feather ESP32-S3 TFT, the small board
idf.py build
esptool.py --port /dev/cu.usbmodem* write_flash 0x10000 build/screen.bin

# CrowPanel 7.0, the big board
idf.py -B build-crowpanel \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.crowpanel" \
  -D SDKCONFIG=sdkconfig.crowpanel build
esptool.py --chip esp32s3 --port /dev/cu.usbserial-* write_flash \
  0x10000 build-crowpanel/screen.bin
```

## Traps, each of which cost an hour or more

**Never raise the baud rate.** `--baud 921600` and `460800` both fail, on the
Feather's USB-Serial-JTAG *and* the CrowPanel's CH340, in different ways:
`No serial data received`, `Invalid head of packet`, or a truncated read. Use
the default. A 4 MB dump at 115200 also drops bytes near the end, so read
flash in chunks with retries rather than in one pass.

**Flash the app only, at `0x10000`, on the Feather.** `idf.py flash` would
also write the bootloader and partition table, destroying TinyUF2 — the
double-tap-RESET recovery drive. `idf.py` even *suggests* `0x2d0000` for the
app, because the partition table labels the TinyUF2 slot `factory`. Ignore it.
The CrowPanel has no TinyUF2, so it takes all three images.

**Check the binary's timestamp before flashing.** A failed build leaves the
previous `screen.bin` in place, and esptool will happily flash it. This has
produced more than one "my change did nothing" hunt.

**Check the image size moves when it should.** A config that silently fails
compiles and runs perfectly — `font.h` once selected the wrong font because it
tested `CONFIG_SCREEN_BOARD_CROWPANEL_7` without including `sdkconfig.h`, and
the only symptom was the binary not growing by the expected 9 KB.

**The CrowPanel's serial console is unreliable.** Bootloader output garbles a
few lines in, reproducibly. It is the CH340, not the firmware. Debug by
drawing on the screen instead — `gt911_debug()` exists for exactly that, and
is how the touch controller was diagnosed.

**GPIO19 and GPIO20 are USB D− and D+ on the ESP32-S3.** The CrowPanel wires
the touch I²C to them, so `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` is required or
the USB peripheral holds the pins.

**`i2c_master_transmit_receive` takes milliseconds, not ticks.** Passing
`pdMS_TO_TICKS(20)` yields 2 at a 100 Hz tick rate, which the driver rounds to
a zero-tick wait; it then returns before the transaction completes and reports
`ESP_ERR_INVALID_STATE`. Every read failed while the address probe, passed a
plain `100`, succeeded — which made the chip look present but unreadable.

**Probe an address nothing should answer.** A stuck-low SDA ACKs every
address, so "the probe succeeded" is not proof a device is there. Two
hypotheses were built on that assumption before it was checked.

## The DS3231 real-time clock

A DS3231 module sits on the CrowPanel's I²C header, which is the touch bus
(**SDA 19, SCL 20**); it answers at `0x68`, the GT911 at `0x5D`, so nothing
collides. `main/ds3231.c` reads it at boot, writes every sync from a Mac back
to it, and re-reads it hourly to cancel the ESP timer's drift. Only the time
of day is stored, with a dummy date, because that is all the board deals in.
The driver is named after the chip because ESP-IDF already owns the symbol
`rtc_init`.

**Check the battery before powering the module.** These modules are built for
a rechargeable LIR2032 but are commonly sold with a non-rechargeable CR2032 in
the holder, which the charging circuit will then try to charge. Fit a LIR2032,
or remove the charging resistor — usually the one marked `201` beside the
diode. A flat battery shows up as the chip's oscillator-stopped flag, and the
firmware then ignores its time until the next sync.

## Architecture, in one paragraph

The firmware owns all drawing; Macs send data, never pixels. A full frame is
750 KB against BLE's ~10–20 KB/s, so sending pictures was never viable.
Payloads are marker-prefixed text lines (`!stats`, `!year`, `!now`, and so
on) on the Nordic UART Service, which means a page can be typed by hand from
nRF Connect to tell firmware bugs from client bugs. The parser is a small
core (`usagedata.c`) plus one `ud_*.c` per payload, registered in
`ud_sections.c`; the renderer is `view_common.c` plus one `view_*.c` per
page; `pagedefs.c` is the table `main.c` reads for what each page is. All of
that, with `textwrap`, `timecalc`, `pages`, `settings` and `canvas`, is pure
C and host-tested; `display_*`, `gt911`, `ds3231` and `ble_uart` are the only
units that touch hardware.

```sh
make -C host_tests && for t in host_tests/test_*; do [ -x "$t" ] && "$t"; done
```

## Double-tapping a menu tile

A tile on the menu opens its page on a single tap and strikes it off the
screensaver's round on a double tap. The struck-off set is a bit per page in
NVS under `cyc_off`, and `cycle_skip()` in `main.c` ORs it into the pages the
saver already refuses to visit.

**The single tap is deliberately late.** It cannot fire until the double-tap
window (`MENU_DOUBLE_US`, 400 ms) has passed without a second tap, because
the alternative -- open the page, then undo it when the second tap arrives --
flashes a page nobody asked for. The delay is paid on the menu alone. The
tile lights up the instant it is touched, so the wait is something you can
see rather than lag you feel.

**Only pages the saver would visit can be struck off.** `toggle_cycle()`
refuses pages whose `in_saver` is false, because setting a bit that changes
nothing would still draw a mark that claims it did.

## The limits page talks to the API

Every other page is derived from files on this Mac. `!limits` is not: it is
the account's rate limits, which live only behind Claude Code's `/usage`
endpoint, so `render_limits` in `tools/claude-stats.py` fetches
`https://api.anthropic.com/api/oauth/usage` with the OAuth token from the
login keychain.

Two things about that are worth knowing before you touch it.

**It goes through `curl`, not `urllib`.** A stock macOS Python has no CA
bundle of its own, so every `urllib` request to that host fails to verify;
`curl` uses the system trust store and just works. The token is written into
a `curl` config file on stdin rather than passed as an argument, so it never
appears in the process list.

**Every failure is quiet in the payload and loud on stderr.** No token, an
expired one, no network: `fetch_limits` returns `None` and the payload goes
out as a bare marker, because this runs on a timer with no one watching and a
collector that raised would take the other thirteen sections down with it. But
every one of those paths calls `limits_note`, which writes the reason to
stderr and so into the agent's `.err` log. The first time this failed silently
it cost an afternoon: the board showed an empty page, the log said "pushed",
and nothing anywhere said why. Reasons are cheap.

The firmware treats an empty payload as "nothing to say" rather than "nothing
is left": `ud_limits.c` counts the rows down to nothing in `begin` and puts
them back in `end` if none arrived, so a failed fetch does not blank the page.

**Do not ask the endpoint every minute.** It answers 429, and a rate-limited
fetch is indistinguishable from an account with no limits. `cached_usage`
asks at most every `LIMITS_MIN_INTERVAL` (five minutes) and backs off for half
an hour after a 429, serving `~/.cache/tell/limits.json` in between. That is
safe here only because the resets are absolute instants: a countdown
recomputed from an old reading is still right to the second.

The reset times are converted to *seconds remaining* before they are sent.
The board has no calendar, only an uptime and a clock it was told, so a
wall-clock reset would need timezone and date arithmetic on the firmware
side. Seconds remaining need a subtraction, and the arithmetic that matters
already happened on a machine that knows what day it is.

The subtraction has one trap in it, and it was fallen into: the section keeps
its own `at_us` rather than reading the host's `updated_us`. The latter moves
whenever *any* section arrives from that machine, so a countdown dated from it
was reset to its pushed value every time the live section landed -- once a
minute -- and read as a clock that would not tick.

## The Feather's IMU

The small board has a QMI8658 on its STEMMA QT bus at `0x6B` — SDA 42, SCL 41,
powered from GPIO21 along with the panel — left over from its stock firmware.
The I²C pins are board dependent, `CONFIG_SCREEN_I2C_SDA`/`_SCL`, because the
CrowPanel wires its GT911 to GPIO19/20 instead.

It drives two pages, neither of which exists on the big board:

| | |
|---|---|
| `!level` | a bullseye spirit level that doubles as a game, since holding a board flat by hand is harder than it sounds |
| `!particles` | a bottle of sand that pours as you tilt it, scatters when shaken and stirs when spun |

`main/level.c` takes two angles and `main/particles.c` a gravity vector; both
are pure C, so they are tested on the host like everything else here. Only the
axis mapping needed the board. The sand keeps its own fast gravity filter in
`main.c`, separate from the level's slow one: the liquid wants to feel the
board move, and an instrument wants to be read.

**The big board is not compiled with any of it.** `main/CMakeLists.txt`
excludes `qmi8658.c`, `level.c` and `particles.c` from the CrowPanel build the
same way it excludes the wrong display driver. What remains shared is two rows
in `pagedefs.c` and five rows in `ud_sections.c`; `main.c` clears both page
bits on any board without the sensor, so the pages are never offered and the
markers do nothing.

**The axis signs cannot be worked out from a still reading.** Gravity has no
component along an axis that is level, so the only way to know is to tilt the
board and look — which is a poor thing to need a reflash for, and they were
wrong twice before it stopped being one. `!flip x`, `!flip y`, `!flip swap`
and `!flip reset` change the mapping and the board remembers it in NVS.

**`!newgame` wipes the scoreboard.** The times live in NVS and outlive a
reflash, so without it a score set before the rules changed can only be
cleared by erasing the partition.

**`!zero` takes the surface the board is on as true.** A desk is not a
reference plane and a hand-mounted breakout is not square to the panel: this
board reads about 3.7° off on one axis wherever you put it, which is the
sensor, not the table. `!zero reset` goes back to absolute.

There is also a **BMP280 at `0x77`**, id `0x58` — where the stock firmware's
temperature came from. Nothing reads it; a driver for it is in the history if
it is ever wanted.

## Adding a page

The firmware is one file per page and one file per payload, each registered
in a table, so a new page touches nothing that exists. In order:

1. **Payload.** Add `!name` to `tools/claude-stats.py` (`render_name`, and its
   entry in the `--all` list and `--section` choices), and a row for it in
   `tools/push-stats.sh`'s section loop. Keep any per-day series to one
   character per day (`grid_char`, or `pct_char` for percentages) so the
   payload stays under the 2048-byte BLE message.
2. **Parser.** Add the per-machine struct and its view struct to
   `main/usagedata.h`, a `UD_NAME` kind (before `UD_KIND_COUNT`), and a
   `main/ud_name.c` with `begin` (reset one machine's copy), `line` (one
   row: `tag`, rest -- return, never `continue`), `end` (validate), `merge`
   (fold every machine's copy into the view) and `used` (has this machine
   sent it), ending in a `const ud_section_t ud_section_name`. Register it in `main/ud_sections.c`.
   Test it in `host_tests/test_usagedata.c`; the helpers you want are in
   `main/ud_internal.h`.
3. **Page.** Add `PAGE_NAME` to the enum in `main/pages.h` where it should sit
   in the tap order, and `main/view_name.c` with
   `void views_name(canvas_t *, const ud_view_t *, float t, int64_t now_us)`
   (declare it in `views.h`; `t` grows bars from 0 to 1 and can be ignored).
   `main/view_common.h` has the title bar, footer, number and date formatting,
   the heat cell and the step plot. Add one row to `main/pagedefs.c`: the menu
   name, `UD_FEED(UD_NAME)`, the draw function, and whether it animates,
   refreshes on its own, exists on the Feather, needs touch, or joins the
   screensaver cycle.
4. **Check it.** `make -C host_tests && ./host_tests/test_views /tmp/page
   payload.txt` renders every page to PPM; `sips -s format png` and look.
   Then `idf.py -B build-crowpanel ... reconfigure` (new files are only seen
   at configure time), build, and `tools/flash-crowpanel.sh`.

`main.c` should not need editing: it reads `page_defs` for availability,
redraw-on-payload, drawing and the saver cycle, and `usagedata_parse` finds
the section by its marker.
