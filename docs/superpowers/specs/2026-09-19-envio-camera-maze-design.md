# envio: camera capture and a tilt-maze

**Date:** 2026-09-19
**Board:** envio — Waveshare ESP32-S3-Touch-LCD-3.5B (AXS15231B QSPI, AXP2101,
OV5640 on the 24-pin DVP header, microSD, QMI8658 IMU soldered on)
**Status:** design, approved in chat; pending spec review before planning.

## Goal

Add two hand-driven pages to envio, which today is a portrait weather + air
dashboard that cycles Clock → Forecast → Climate → Air every 20 s:

1. **Camera** — a viewfinder while the page is held, tap to save a JPEG to the
   microSD.
2. **Maze** — a tilt-a-ball game on the soldered QMI8658, with a run timer and
   a few hand-drawn levels.

Both are **reachable by swipe but excluded from the auto-cycle**, so the
dashboard keeps rotating the readings and neither the camera nor the game ever
appears on its own. Neither touches the other boards (lilly, the CrowPanel):
all of this is gated to the 3.5B.

## Non-goals

- No live streaming, Wi-Fi upload, video, or gallery browser. Capture writes a
  file; reviewing the shots happens off the card.
- No board-to-board anything ([[boards-stay-independent]] holds).
- No new sensor. The maze uses the QMI8658 already on the board.

## Hardware map

From Waveshare's documentation for this board (wiki + demo). Pins are taken
from the vendor source, not guessed — the recurring lesson from lilly and wave.

### DVP camera (OV5640)

| Signal | GPIO | Signal | GPIO |
|--------|------|--------|------|
| XCLK   | 38   | PCLK   | 41   |
| VSYNC  | 17   | HREF   | 18   |
| D0 (Y2)| 45   | D4 (Y6)| 42   |
| D1 (Y3)| 47   | D5 (Y7)| 40   |
| D2 (Y4)| 48   | D6 (Y8)| 39   |
| D3 (Y5)| 46   | D7 (Y9)| 21   |
| PWDN   | -1   | RESET  | -1   |

**SCCB (camera control) = SIOD 8 / SIOC 7 — the same two pins as envio's main
I2C bus.** The AXP2101 (0x34), the AXS15231B touch (0x3B), the QMI8658, the
DS3231 (0x68), the AHT21 (0x38), the ENS160 (0x53) and the TCA9554 expander all
sit on GPIO8/7 already. The OV5640's control channel is that same bus. This is
the single largest integration risk and is treated as its own section below.

None of the DVP data/clock pins collide with the QSPI panel (CS 12, SCLK 5,
D0-3 = 1/2/3/4) or the backlight (LEDC on 6).

### microSD

1-bit SDMMC (`SD_MMC.setPins(clk, cmd, d0)` in the demo). **The three GPIO
numbers are not on the wiki page.** They are read from Waveshare's demo
download (or the schematic) as the first implementation step, and recorded in
[[envio-board]] once known. They are not guessed and not hard-coded until
confirmed. The board is in hand, so a wrong guess would be caught, but the
whole point of the pin discipline is not to guess.

## SCCB / I2C coexistence (the crux)

envio owns GPIO8/7 through the project's `i2cbus` (`I2CBUS_MAIN`), created once
at boot and shared by every I2C driver. `esp32-camera` normally initialises its
own SCCB on the pins it is handed, which would mean two masters configuring the
same pins.

**Primary approach:** hand `esp32-camera` the *existing* bus rather than pins.
`camera_config_t` exposes `sccb_i2c_port`; setting it to the port `i2cbus`
already opened, and leaving `pin_sccb_sda`/`pin_sccb_scl` at -1, makes the
camera ride the shared bus instead of re-initialising it. The camera init must
therefore run **after** `i2cbus` is up (it already is — sensors are probed
before the page list is built).

A compatibility check gates this: `esp32-camera`'s SCCB layer must target the
same ESP-IDF I2C driver generation as `i2cbus`. If `i2cbus` is on the new
`i2c_master` driver and the pinned `esp32-camera` release still uses the legacy
`driver/i2c` SCCB, sharing a port is not possible as-is.

**Fallback if they cannot share a port:** let `esp32-camera` own GPIO8/7 and
create the SCCB bus, then route the other I2C devices through the handle it
exposes. This is more invasive (every sensor's bus acquisition changes) and is
only taken if the primary approach fails on hardware. The choice is made early,
during the camera bring-up spike, before the page work.

Either way, the sensor bus is busy only while the Camera page is active (see
lifecycle); during the normal dashboard the camera is deinitialised and the bus
carries only the usual sensor traffic.

## Camera subsystem — `main/camera.c` / `camera.h`

A small wrapper over `esp32-camera`, envio-only, so nothing else in the tree
depends on the managed component being linked.

### Lifecycle — lazy, tied to the page

- **On entering PAGE_CAMERA:** `camera_start()` → `esp_camera_init` in RGB565,
  QVGA (320×240), `fb_location = CAMERA_FB_IN_PSRAM`, `fb_count = 1`, XCLK ~20
  MHz, `sccb_i2c_port` = the shared port.
- **On leaving PAGE_CAMERA:** `camera_stop()` → `esp_camera_deinit`, which frees
  the PSRAM frame buffer and stops XCLK, powering the sensor down. This keeps
  the camera off during the auto-cycle and off battery budget when unused — the
  reason the still-first design was chosen over an always-on viewfinder.

Because Camera and Maze are excluded from the auto-cycle (next section), "the
Camera page is current" is equivalent to "held by the user", so the viewfinder
only ever runs when someone is looking.

### Viewfinder

While PAGE_CAMERA is current, the main loop grabs a frame each iteration
(`esp_camera_fb_get`), and blits the RGB565 buffer to the panel through the
existing canvas/QSPI band path, scaled/letterboxed to the 320×480 portrait
(QVGA is 320×240, so it centres with room for a caption line). `esp_camera_fb_return`
every frame. Target is "smooth enough to aim", not a frame-rate guarantee; the
QSPI panel and PSRAM are shared, and the spec accepts whatever fps that yields.

### Capture (tap = shutter)

`axs_touch` already classifies a tap versus a horizontal swipe. On the Camera
page a **tap** is the shutter; a horizontal **swipe** still pages away as
everywhere else.

1. Reconfigure the sensor to JPEG at a higher resolution (target SVGA/UXGA,
   chosen for a clean JPEG the sensor encodes in hardware).
2. One `esp_camera_fb_get`, write the JPEG bytes to SD, `fb_return`.
3. Reconfigure back to the RGB565 QVGA preview.

The reconfigure costs a fraction of a second — acceptable as shutter lag, and
only paid when actually taking a shot. A brief on-screen "Saved IMG_…"
confirmation is shown before the viewfinder resumes.

## Storage — microSD

- Mounted 1-bit SDMMC, lazily (on entering the Camera page or on first
  capture), via `esp_vfs_fat_sdmmc_mount` at `/sdcard`.
- Files written to `/sdcard/envio/`, named `IMG_YYYYMMDD_HHMMSS.jpg` from the
  DS3231 clock (envio keeps real time across power cycles). If the clock is not
  yet set, fall back to a boot-relative counter so a shot is never lost.
- SD errors (no card, mount fail, write fail) surface as a one-line message on
  the Camera page and leave the viewfinder running; they never crash the page
  or stall the dashboard.

## Page model

Two new pages appended to the `page_t` enum, drawn by new dispatch arms in
`main.c`, with rows in `pagedefs.c`:

- `PAGE_CAMERA`
- `PAGE_MAZE`

**Availability vs rotation.** envio builds its `available` mask, and the
auto-cycle already runs through `pages_tick`/`pages_step` with a `skip` mask
(the same one the screensaver uses). Camera and Maze go **into `available`**
(so `pages_advance`/`pages_back` — the swipes — reach them) and **into the
`skip` mask** (so the 20 s rotation and the saver leave them out). This is
exactly the mechanism `pages.h` documents; no new page machinery is needed.

Home stays the clock; the cycle stays Clock → Forecast → Climate → Air. Swiping
past Air reaches Camera then Maze then wraps.

## Maze — `main/maze.c` / `maze.h`

A self-contained tilt-a-ball page modeled on the existing `pip.c` (which
already reads the QMI8658 and reacts to gravity), using envio's blue/amber/white
palette.

- **Re-enable the IMU on envio:** drop `qmi8658.c` (and the shared-math helpers
  it needs) from the 3.5B `BOARD_EXCLUDES` in `CMakeLists.txt`. It is soldered
  on; only the build currently omits it. `level.c`/`particles.c`/`shaketimer.c`
  stay excluded unless a level needs them.
- **Model:** a ball with a floating position and velocity; gravity from the
  accelerometer accelerates it (same axis mapping the level/particles code
  established for this panel); walls are a hand-authored grid; the ball collides
  with walls and the screen edge; reaching the goal cell advances to the next
  level. A run timer counts up and is shown; finishing the last level shows the
  total time.
- **Levels:** a few static, hand-drawn grids compiled in (a header of bitmaps,
  like the fonts). Start with 3.
- **Controls:** tilt to roll. A tap resets the current level. Swipe pages away.

The maze is independent of the camera and can be built and tested first — it
has no external hardware unknowns.

## Build and configuration

- **`main/idf_component.yml`:** add `espressif/esp32-camera`. It links for the
  build but is only *called* from `camera.c`.
- **`main/CMakeLists.txt`:** compile `camera.c` and `maze.c` on the 3.5B only
  (the existing per-board `BOARD_EXCLUDES` pattern); exclude them elsewhere.
  Remove `qmi8658.c` from the 3.5B excludes.
- **Kconfig (`Kconfig.projbuild`):** `CONFIG_SCREEN_HAVE_CAMERA`, defaulted on
  for `SCREEN_BOARD_TOUCH_LCD_35B`, guarding the camera page and its includes so
  a board without the DVP header never pulls it.
- **PSRAM budget:** the QSPI framebuffer draws in bands from PSRAM; one QVGA
  RGB565 preview buffer is ~150 KB and a JPEG capture buffer is transient. The
  8 MB octal PSRAM (eFuse AP_3v3, already enabled) has room; the bring-up
  confirms no allocation failure with both the panel and the camera live.
- **`sdkconfig.defaults.envio`:** enable FAT/SDMMC VFS as needed; keep the
  camera's own Kconfig (JPEG, PSRAM fb) consistent with the wrapper.

## Risks and open items

1. **SCCB shared-bus coexistence** — primary (shared `sccb_i2c_port`) vs
   fallback (camera owns the bus). Resolved during a short bring-up spike before
   the page work; the fallback is more invasive but known.
2. **SD pin numbers** — read from Waveshare's demo download/schematic as the
   first step; recorded in [[envio-board]]; never guessed.
3. **esp32-camera ↔ QSPI/PSRAM contention** — preview fps is best-effort. If it
   is unusably slow, the fallback is a lower preview rate or a snapshot-preview
   (grab one frame per tap-to-focus) rather than a continuous stream; the
   still-capture path is unaffected either way.
4. **esp32-camera I2C driver generation** vs `i2cbus` — checked before relying
   on port sharing (see the coexistence section).

## Order of work (for the plan)

1. Maze first — no external unknowns; proves the IMU re-enable and the
   swipe-not-cycled page slot end to end.
2. Camera bring-up spike — SD pins, SCCB coexistence, a single still to SD.
3. Viewfinder and the capture/reconfigure cycle.
4. Polish: confirmation UI, error handling, palette, page order.
