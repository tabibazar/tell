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

## Next: the DS3231 real-time clock

A Wishiot **DS3231 + AT24C32** module is to be wired to the CrowPanel, giving
the board a battery-backed clock. Today it starts at `--:--:--` after every
power cycle and drifts until a `tell` re-syncs it; the DS3231 is
temperature-compensated to about a minute a year.

**Wiring:** the same I²C bus as the touch controller — **SDA 19, SCL 20**,
3V3 and GND. DS3231 answers at `0x68` and the AT24C32 EEPROM at `0x50`–`0x57`;
the GT911 is at `0x5D`, so nothing collides.

**The blocking refactor is already done.** `main/i2cbus.c` owns the bus and
drivers attach to it with `i2cbus_handle()` and `i2cbus_probe()`. Before that,
`gt911_init()` created the bus itself and a second `i2c_new_master_bus` on the
same port would have failed. So this is now a driver plus wiring.

**Check the battery before powering it.** These modules are built for a
rechargeable LIR2032 but are commonly sold with a non-rechargeable CR2032 in
the holder, which the charging circuit will then try to charge. Fit a LIR2032,
or remove the charging resistor — usually the one marked `201` beside the
diode.

## Architecture, in one paragraph

The firmware owns all drawing; Macs send data, never pixels. A full frame is
750 KB against BLE's ~10–20 KB/s, so sending pictures was never viable.
Payloads are marker-prefixed text lines (`!stats`, `!daily`, `!clock`,
`!today`) on the Nordic UART Service, which means a page can be typed by hand
from nRF Connect to tell firmware bugs from client bugs. `usagedata`,
`textwrap`, `timecalc`, `pages` and `canvas` are pure C and host-tested;
`display_*` and `gt911` are the only units that touch hardware.

```sh
make -C host_tests && for t in host_tests/test_*; do [ -x "$t" ] && "$t"; done
```
