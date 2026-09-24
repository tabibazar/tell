# envo field sleep: log every 30 s, sleep in between

2026-09-23. envo only (`CONFIG_SCREEN_BOARD_TOUCH_LCD_147`).

## Goal

envo goes into the field for about three months on a USB power bank, with no
Mac. It must keep logging temperature, humidity and pressure every 30 s to the
SD card, and run as cool as it can: the chip near room temperature, not the
~57 C it sits at now. The screen is dark unless someone taps it.

## Findings that shaped it

- The heat is the chip being awake all the time (160 MHz with the idle task
  spinning, then 80 MHz under DFS, BLE up, panel lit), not the sampling.
- Only GPIO0-21 can wake the S3 from deep sleep. The touch interrupt is
  **GPIO48** (checked on the bench: it pulses low for under 40 ms on every
  tap), so deep sleep could only be woken by the button. Light sleep can be
  woken by any GPIO and keeps RAM, so nothing has to be rebuilt on waking.
- BLE cannot coexist with light sleep here (the controller says so at boot).
  envo has no use for it in the field: the DS3231 keeps the time.

## Design

**Awake (as now).** Full UI, touch, 30 s SD log, 5 min flash ring. The saver
timeout defaults to **5 minutes** on envo, not 10.

**Field sleep.** Where the saver would start, envo instead:
1. Turns the backlight off (duty 0) and puts the panel to sleep
   (display off + sleep-in), via a new `display_sleep(bool)` in display.h
   (a no-op on the other display drivers).
2. Loops: arm a timer wake for the next 30 s log slot, plus GPIO wake on
   GPIO48 (touch INT, low) and GPIO0 (button, low); call
   `esp_light_sleep_start()`.
   - **Timer wake:** run the existing `env_sdlog()` / `env_sample()` (their
     cadence is keyed on `esp_timer`, which carries on through light sleep),
     plus the hourly RTC re-read, then sleep again. No drawing.
   - **Touch/button wake:** leave field sleep. The panel wakes, the backlight
     comes back to `CONFIG_SCREEN_BRIGHTNESS`, the activity clock resets, and
     the waking tap is swallowed, as the saver already does.
3. **Not while a USB host is attached.** If `usb_serial_jtag_is_connected()`
   sees a host (a Mac, not a power bank), envo just goes dark without
   sleeping. This keeps the serial console and flashing working at the desk;
   a sleeping chip has no USB, so esptool could not reach it. A power bank
   sends no USB frames, so in the field it sleeps.

**BLE off on envo.** `ble_uart_start()` is skipped for this board and
`tools/flash-envo.sh` stops pushing the clock (`push-clock.sh` needs BLE).
The DS3231 is the only clock; if its oscillator-stop flag is set, logging
already waits rather than writing a false date.

**Proof in the log.** The SD CSV gains a trailing `die_c` column (the chip's own
temperature), so the field log itself shows whether the chip ran cool. The
column is appended at the end; the existing columns do not move.

## Out of scope

Deep sleep; the power bank's low-current auto-off (a hardware choice; see the
conversation); the dead ENS160 and the MiCS-5524 (separate work).

## Testing

- Host: none new. Nothing here is pure logic worth a host test.
- Bench, Mac attached: it goes dark at 5 min and does *not* sleep; a tap
  wakes it; serial keeps working; the CSV carries `die_c`.
- Bench, on a power bank or charger (no host): let it go dark; after an hour
  check that
  - the SD file has one line per 30 s with no gaps,
  - `die_c` settles near room temperature,
  - a tap wakes the screen promptly,
  - the time is still right.
- Regression: build lilly and wave to confirm the no-op `display_sleep` and the
  board guards compile.
