# Tilt-driven particles on the Feather — design

The `small` board — an Adafruit Feather ESP32-S3 TFT — shows a clock and
whatever `tell` last sent it, and after five idle minutes a clock that hops to a
new spot each minute so no pixel burns in. It also has a QMI8658 inertial
measurement unit attached, unused since we replaced its stock firmware.

This gives it a physical simulation: a few hundred particles that fall towards
whichever way the board is actually tilted, drawn on the 240x135 panel. It
replaces the drifting clock as the Feather's screensaver, because particles in
motion protect the panel better than a clock that moves once a minute, and
because an object on a desk should do something worth looking at.

## Scope

In: a QMI8658 driver, a host-tested particle simulation, a new page, and the
board-conditional I2C wiring the driver needs.

Out: the CrowPanel, which has no IMU and whose page set does not change. Out:
any use of the gyroscope beyond an optional swirl term. Out: interaction — the
particles respond to the board being tilted and to nothing else.

## The hardware, as established

The IMU is real and was working before we reflashed the board. The stock image
in `~/esp32-firmware-backup/stock-tabriz.bin` still contains:

```
[%s] ERROR! ID NOT MATCH QMI8658 , Respone id is 0x%x
Failed to find QMI8658 - check your wiring!
{ACCEL:
{GYRO:
```

and the running board printed `Device ID:7B` at us on day one, which is the
QMI8658's revision register.

**Where it is wired is an assumption.** Nothing in this repository records it.
The working assumption is the STEMMA QT connector, whose pins Adafruit's board
variant gives as **SDA 42, SCL 41** — the same file that supplies every display
pin this project already uses, so it is corroborated rather than recalled. That
port is powered from `TFT_I2C_POWER` on GPIO21, which `display_init()` already
drives high for the panel, so a QT-connected sensor is powered for free.

The assumption is verified before anything is built on it. See "Bring-up".

The Feather keeps its USB-Serial-JTAG console
(`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in `sdkconfig.defaults`), so unlike the
CrowPanel this board can be debugged over serial in the ordinary way.

## Architecture

Four units, each with one job, following the shape the project already has.

**`main/i2cbus.c` — modified.** It owns the single I2C bus, and today it
hardcodes SDA 19 / SCL 20, which is CrowPanel wiring inherited from the GT911.
The pins become Kconfig integers with per-board defaults, the way
`SCREEN_DEVICE_NAME` already does:

```
config SCREEN_I2C_SDA
    int "I2C SDA pin"
    default 19 if SCREEN_BOARD_CROWPANEL_7
    default 42
config SCREEN_I2C_SCL
    int "I2C SCL pin"
    default 20 if SCREEN_BOARD_CROWPANEL_7
    default 41
```

`i2cbus.c` gains an explicit `#include "sdkconfig.h"`. This is not optional
tidiness: `font.h` once tested `CONFIG_SCREEN_BOARD_CROWPANEL_7` without that
include, compiled clean, ran perfectly, and selected the wrong font.

**`main/qmi8658.c/.h` — new, hardware.** Modelled on `gt911.c`, which is the
only other I2C driver here.

```c
esp_err_t qmi8658_init(void);            /* probe, identify, configure */
bool      qmi8658_present(void);
esp_err_t qmi8658_read(qmi8658_sample_t *out);
```

where `qmi8658_sample_t` carries six floats — accelerometer in g, gyroscope in
degrees per second. The driver attaches to `i2cbus_handle()` and never creates a
bus of its own; a second `i2c_new_master_bus` on the same port fails, which is
why the bus was factored out in the first place.

Every timeout argument to `i2c_master_transmit_receive` is a plain integer of
**milliseconds**. Passing `pdMS_TO_TICKS(20)` yields 2, which the driver rounds
to a zero-tick wait, returns before the transaction completes, and reports as
`ESP_ERR_INVALID_STATE`. That cost most of an afternoon on the GT911.

Configuration: address auto-increment on, accelerometer at +/-4 g and about
250 Hz, gyroscope at +/-512 dps, both enabled. The register numbers are written
against the QMI8658 datasheet and confirmed during bring-up by reading
WHO_AM_I before anything else is trusted.

**`main/particles.c/.h` — new, pure C, host-tested.** No ESP-IDF headers, so it
builds and runs under `host_tests/` alongside `canvas`, `timecalc` and
`usagedata`.

```c
void particles_init(particles_t *p, int n, int w, int h, uint32_t seed);
void particles_step(particles_t *p, float gx, float gy, float dt);
void particles_draw(const particles_t *p, canvas_t *c);
```

Positions and velocities are floats in panel coordinates. `gx`/`gy` are a
gravity vector in panel space, already low-passed by the caller; `particles.c`
knows nothing about accelerometers and can therefore be tested by handing it a
constant vector. Walls bounce with a restitution below one and there is a small
drag term, so the system settles into a heap instead of jittering forever.
Particles do not collide with each other — a few hundred of them at 20 frames a
second on a 240 MHz core does not have the budget, and a sandpile that slumps
under gravity reads correctly without it.

**`main/main.c` — modified.** A new `PAGE_PARTICLES`, added to `pages.h` and
made available only under `CONFIG_SCREEN_BOARD_FEATHER_S3_TFT`, and only when
`qmi8658_present()` is true. The existing tick loop drives it: read the IMU, map
its axes into panel space, low-pass, step, draw, blit.

## How the page is reached

The Feather has no touch controller, so the two ways in are both indirect.

**As the screensaver.** `main.c` currently branches on `s_saver` and calls
`draw_saver()`, which places the clock at a pseudo-random spot each minute. On
the Feather, when the IMU is present, that branch draws particles instead. After
five idle minutes the board starts its simulation on its own and keeps running
until a message or a `tell` arrives. This is the primary path and requires
nothing of the user.

**On demand, over BLE.** A `!particles` marker, handled beside `!clock`,
`!stats`, `!daily` and `!today`, pins the page for the usual `PAGES_IDLE_US`
five minutes. This matches the protocol the whole project uses — a page that can
be summoned by typing a line into nRF Connect is a page whose firmware can be
told apart from its client.

If the IMU is absent or fails to initialise, neither path is offered and the
drifting clock stays exactly as it is. A board with no sensor loses nothing.

## Axis mapping and filtering

Which accelerometer axis points down the screen depends on how the breakout sits
relative to the panel, and that cannot be known from a datasheet. Bring-up draws
the raw vector on the panel and the mapping is chosen from what is observed:
tilting the board left must move the particles left.

The mapping is then a fixed sign-and-swizzle constant in `main.c`, not a runtime
setting. Raw samples are low-passed with a first-order filter of roughly a
150 ms time constant, because raw accelerometer output at rest is noisy enough
to make a settled heap shiver.

The gyroscope contributes at most a swirl term — angular velocity about the axis
normal to the screen, added as a tangential nudge. It is a garnish. If it does
not look good it is removed, and nothing else depends on it.

## Frame budget

A full frame is 240 x 135 x 2 = 64,800 bytes. At the 40 MHz SPI clock in
`display_st7789.c` that is about 13 ms of transfer, and the main loop already
ticks every 50 ms, which is 20 frames a second. That is the starting point.

One blit is measured during bring-up before the particle count is chosen. If
there is headroom the loop's cadence drops to about 33 ms while particles are
running and returns to 50 ms otherwise, so nothing else on the board is made
busier than it is today. NimBLE runs on its own task, so the animation cannot
delay a `tell`; that `pages_tick` still runs on schedule is checked explicitly.

Particle count starts at 200 and is tuned against the measurement. Colour comes
from `palette.h`, so the animation belongs to the same Okabe-Ito set as the
charts.

## Bring-up, and where it stops

The first task builds nothing but an I2C scan, because three days of carrying
the board around is ample opportunity for a sensor to have been unplugged.

1. Bring the bus up on SDA 42 / SCL 41 and scan every address.
2. Probe an address nothing should answer. A stuck-low SDA acknowledges
   everything, so a successful probe is not by itself proof a device is there.
   Two hypotheses were built on that assumption during the GT911 work before it
   was checked.
3. Expect the QMI8658 at 0x6A or 0x6B. Read WHO_AM_I, which must be 0x05, and
   the revision register, which should return the 0x7B the stock firmware
   printed.

Output goes to the serial console, which works on this board.

**If nothing answers, work stops there** and the question of where the IMU is
wired comes back to Reza. No part of the simulation is written against a sensor
that has not answered.

## Testing

`particles.c` is tested on the host, in `host_tests/`, in the project's existing
style:

- particles stay within the panel bounds over a thousand steps, under gravity
  pointing in each of four directions
- total kinetic energy decays when gravity is zero, so the damping is real
- with gravity pointing right, the centre of mass moves right
- `particles_draw` writes only inside the framebuffer, checked with guard bytes
  either side, the way `canvas` is already tested

The driver and the wiring are tested on the board, because they cannot be tested
anywhere else: the scan identifies the part, the raw-vector display confirms the
axis mapping, and the finished animation is judged by looking at it.

## Constraints that remain in force

Flash the **app only, at `0x10000`** on the Feather. `idf.py flash` also writes
the bootloader and partition table, destroying the TinyUF2 recovery drive.

Never raise the baud rate. Check `build/screen.bin`'s timestamp before flashing,
because a failed build leaves the previous image in place. Check that the image
grows when these new units land — a config that silently fails compiles and runs
perfectly.
