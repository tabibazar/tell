# watch: a grand-complication clock with the sand

2026-09-24. Board `watch`: Waveshare ESP32-S3-Touch-LCD-1.69, MAC
80:45:6b:35:11:d4, N16R8 (16 MB flash, 8 MB PSRAM).

## Goal

An analog watch face worth looking at, not a digital clock: a grand
complication with sub-dials and a moon phase. The sand game from wave is the
second mode. Both buttons get real jobs.

## Decisions (Reza, 2026-09-24)

- Face: **grand complication**.
- Seconds: **quartz tick**, one jump a second; the face redraws once a second.
- Buttons: **watch-style** (below).
- Idle: **raise to wake**; dark after 30 s untouched.

## 1. Bring-up (new board target `watch`)

- `SCREEN_BOARD_TOUCH_LCD_169` in Kconfig; its own CMake excludes,
  `sdkconfig.defaults.watch`, `partitions-watch.csv` and `tools/flash-watch.sh`.
  Guarded by `boards.tsv`, as every board is.
- **Display:** ST7789V2, **portrait 240x280**, in `display_st7789.c`. Rotation
  becomes per-board rather than always landscape. Framebuffer in PSRAM.
- **Power latch:** SYS_EN is driven high as the first thing app_main does, or
  the board powers itself off on battery.
- **Two hardware revisions** differ in pins (buzzer 33/42, RTC INT 41/39,
  SYS_EN 35/41, SYS_OUT 36/40). The revision is established from Waveshare's
  sources and checked on the unit before any pin is trusted.
- **Touch:** new `cst816d_touch.c` (0x15) behind `touch.h`. The panel is
  native portrait, so no axis swap.
- **RTC:** new `pcf85063.c` (0x51) implementing the `ds3231.h` function names,
  so main.c is unchanged. The DS3231-only extras (temperature, aging, stopped)
  return false. The Mac sets it over BLE as for the other boards; the RTC keeps
  it from then on.
- **IMU:** QMI8658, as on wave. **Battery:** ADC on GPIO1, as a percentage.

## 2. Sand

wave's sand at 240x280 portrait:
- `PARTICLES_GRID_H` raised so a 280-row panel is never clamped (>= 29).
- The gravity cap re-tuned for the taller panel.
- The IMU axis mapping defaulted for portrait.

## 3. The face

- **`vector.c`** (new, host-tested): anti-aliased thick lines with round caps,
  filled convex/concave polygons with AA edges, arcs (thick, AA), filled AA
  discs, and blending into RGB565. Clipped to the canvas, so no write can land
  outside it.
- **`moonphase.c`** (new, host-tested): the phase (0..1), the age in days, the
  phase name, and the next full and new moon, from Unix time. The same model as
  `tools/almanac.py` (reference new moon 2000-01-06 18:14 UTC, synodic month
  29.530588853 d), checked against it on fixed dates.
- **`face.c`** (new, host-tested, rendered to an image on the host for review):
  - a dark sunburst dial with cream indices and minute track;
  - leaf-shaped gold hour and minute hands;
  - small seconds at 6 (quartz tick);
  - day-of-week sub-dial at 9 and date sub-dial at 3, each with its own hand;
  - a moon-phase aperture at 12, with the moon disc passing behind two humps;
  - a power-reserve arc for the battery.
  Pure: `face_draw(canvas, const face_state_t *)` with the time, date, weekday,
  moon phase and battery passed in.
- **Moon page** (BOOT on the face): a big moon, its name, its age, and the
  dates of the next full and next new moon.

## 4. Buttons, touch, idle

- **Function button** (on SYS_OUT): a short press toggles face / sand. A long
  press (2 s) releases the latch and powers off on battery; on USB, where it
  cannot, it only darkens the screen.
- **BOOT (GPIO0):** on the face it toggles the moon page; in the sand it
  pours a fresh pile.
- **Idle:** dark after 30 s untouched. The chip stays up (DFS 80-160 MHz) and
  polls the IMU at 10 Hz. A raise/tilt toward the viewer, a touch or a button
  wakes it.

## Testing

- **Host:**
  - `test_vector` (bounds, AA coverage, fill correctness);
  - `test_moonphase` (against `almanac.py` values on fixed dates);
  - `test_pcf85063` (register pack/unpack);
  - `test_face` (bounds, plus a PPM render at several times for eyeballing);
  - `test_particles` at 240x280.
- **Board:** bring-up (panel, latch, touch, RTC, IMU, buttons, battery), then
  the sand, then the face, each flashed and checked on the glass.
- **Regression:** lilly, wave and envo still build.
