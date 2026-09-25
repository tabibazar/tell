# speaker: a noise monitor with a light ring

2026-09-25.

**Board:** speaker, a Waveshare ESP32-S3-AUDIO-Board (MAC 28:84:85:56:f6:b0,
N16R8, no screen fitted).

**Hardware facts, with sources:** `docs/hardware/speaker-fingerprint.md` and
`docs/hardware/speaker-pinout.md`, the reconciled research (schematic v1.1,
the factory demo and xiaozhi).

## Reza's requirements

- **The ring** (7 WS2812):
  - **Fill:** the INSTANT level (fast, 125 ms). More LEDs lit means louder right now.
  - **Colour:** the level smoothed over 3 s (LAeq).
  - **Home/office scale:** green up to 45 dBA, yellow ~55, orange ~65, red from 75 dBA, as a gradient.
- **Night mode:** the ring dims 22:00-07:00, by the RTC, and keeps measuring.
- **NO SOUND 21:00-06:00,** whatever any setting says.
- **Chime:** after 60 s of continuous red.
  - Off by default.
  - At most once per 10 min.
  - Never in the quiet hours.
- **SD card (16 GB):**
  - A detailed log every second.
  - A running daily average in `speaker/daily.csv`.
- **No screen, no Wi-Fi.** BLE lets the Mac set the clock and send settings.
- **The side buttons** stay unassigned until Reza approves a mapping. Presses are logged only.

## Hardware (verified; see the pinout doc)

- **I2S, ESP32 master:** MCLK 12, BCLK 13, WS 14, DOUT 16 (to the ES8311), DIN 15 (from the ES7210).
- **I2C** SDA 11, SCL 10, 7-bit addresses: ES8311 0x18, ES7210 0x40, TCA9555 0x20, PCF85063 0x51.
- **The mics:** two ANALOG MEMS into the ES7210 CH1 (MIC1) and CH2 (MIC2).
  - CH3 is the speaker loopback, about -24 dB: it shows exactly when the chime plays.
  - CH4 is unused.
  - The TDM slot order is CH1, CH3, CH2, CH4.
  - Sensitivity is unpublished, so calibration is mandatory.
- **The ring:** 7 WS2812 on GPIO38 via RMT. Colour order RGB by default; verify on the unit.
- **The TCA9555** (EXIO n = bit n):
  - 0 LCD_RST, 1 TP_RST, 2 TP_INT, 3 SD_D3 (keep an input with a pull-up).
  - 4 RTC_INT, 5 CAM_PWDN, 6 **Camera_SEL: NEVER drive LOW (it disconnects USB)**, 7 NC.
  - 8 **PA_CTRL: speaker amp, HIGH = on**.
  - 9/10/11 Key1/2/3 (inputs, active low); 12-15 free.
- **BOOT key** on GPIO0.
- **SD:** SDMMC 1-bit, CLK 40, CMD 42, D0 41, with D3 on EXIO3 held high by its pull-up.
- **Amp:** NS4150B on the VCC rail. It needs 120 ms after EXIO8 goes high before sound, and 80 ms to shut down.
- **No power-hold pin.** Battery sense is not wired by default.

## Boot order (safety)

1. **WS2812 first.** Start RMT on GPIO38 and send all-off, so the LEDs don't latch random colours.
2. **The TCA9555 through `esp_io_expander` (tca95xx_16bit).**
   - Create it, then `io->write_output_reg(io, 0xFFFF & ~PIN_8)`, then set EXIO8 as an output. The amp is then off with no pulse.
   - Everything else stays an input. EXIO6 is never driven.
   - Read both input ports so INT# is released.
3. **The codecs** through `esp_codec_dev`.
   - I2S full duplex at **16 kHz**, MCLK = 256 x fs.
   - RX in TDM, 4 slots x 16 bit. TX std, 16-bit.
   - ES7210 as a slave: mics MIC1..MIC4 selected (for TDM); PGA **24 dB** on MIC1/MIC2 (headroom for loud rooms); MIC3 is the reference.
   - ES8311: use_mclk = true, volume capped at 60.

## Measurement (`main/soundlevel.c`, pure, host-tested)

- **Input:** MIC1 and MIC2 at 16 kHz, each A-weighted on its own, with their energies averaged. They are not averaged sample by sample: the capsules are 34.5 mm apart, and a sample mean comb-filters sound arriving along the mic axis (-10 dB at 4 kHz), as the review measured.
- **A-weighting:** the IEC 61672 filter as cascaded biquads, designed for fs = 16 kHz. Its response is tested within the standard's class-2 tolerance up to 6.3 kHz.
- **Levels:**
  - **LAF:** the fast level, the RMS over 125 ms blocks.
  - **LAeq,3s:** a ring of 24 blocks.
  - **LAeq,1s:** for the detail log.
  - **Per minute and per day:** LAeq, max, min, L90 (from a 0.5 dB histogram) and red seconds.
- **dBA** = dBFS + `cal_offset`.
  - The default offset is estimated from the ES7210 full scale, the 24 dB PGA and a typical -38 dBV/Pa analog MEMS. It is marked "est" in the log until calibrated.
  - `!cal NN` (over BLE) says "the room is NN dBA now" and sets the offset. It is stored in NVS.
- **Gating:** while the chime plays, and for 150 ms after the amp goes off (the CH3 reference confirms it), blocks are excluded. They are neither logged nor shown.

## The ring (`main/ring.c`, pure, host-tested; led driver in the app)

- **Fill:** LAF from 35 to 85 dBA across the 7 LEDs.
  - The last lit LED is fractional (partial brightness) for smoothness.
  - Below 35 dBA, a single dim LED shows the monitor is alive.
- **Colour:** LAeq,3s through the gradient: green (0,200,60) up to 45, yellow at 55, orange at 65, red (255,0,0) from 75, interpolated in between.
- **Glide:** about 150 ms toward the target, run at 50 Hz.
- **Brightness:** capped at 25 % by default; 8 % in night mode (22:00-07:00).
  - `!ring off` / `!ring on` over BLE.
  - `!night HH-HH` changes the window.

## Chime

- **When:** the LAeq,3s has been red continuously for 60 s.
- **Rules:** only when `chime` is enabled (off by default, `!chime on`), outside the quiet hours 21:00-06:00, and at most once per 10 min.
- **Sequence:**
  1. Codec out with silence.
  2. EXIO8 HIGH, then wait 150 ms.
  3. A soft two-tone chime, about 0.6 s, peaking at -6 dBFS, volume 60 or less.
  4. 60 ms of silence, then EXIO8 LOW.
- **Gating** as above.

## Logging (SD, `speaker/`, stamped by the RTC)

- **Detail:** `speaker/YYYY-MM-DD.csv`, one line per second:
  `time,laeq1s,lafmax1s,cal` (cal = "est" or "cal"). About 3.5 MB a day.
- **Daily:** `speaker/daily.csv`, one line per day:
  `date,laeq_day_so_far,lday_07_19,levening_19_23,lnight_23_07,max,l90,red_minutes,cal`.
  - Today's line is rewritten every minute, via a temporary file and a rename.
  - Past days are kept as they are.
- **Recovery:** after a reboot, today's running totals are restored from the daily line: energy sums and counts in a small sidecar `speaker/today.bin`.
- **No clock yet:** before the RTC has a valid date, nothing is written to SD. The ring still works.

## App structure

- `main/speaker_app.c`: `speaker_app_main()`, called at the top of app_main on this board, which then does not run the page UI. It owns:
  - NVS and BLE (clock push, `!cal`, `!chime`, `!ring`, `!night`);
  - the RTC (reusing pcf85063.c);
  - the SD card;
  - an audio task (core 1, I2S read, soundlevel);
  - an LED task (50 Hz);
  - a 1 s logger;
  - the chime;
  - key polling (log only).
- `main/display_none.c`: stubs so the shared main.c links on a screenless board.
- **Board target:** `SCREEN_BOARD_AUDIO_S3`, device name "speaker",
  `sdkconfig.defaults.speaker` (octal PSRAM, 16 MB), `partitions-speaker.csv`, `tools/flash-speaker.sh` behind the MAC guard.
- **Components:** espressif/esp_codec_dev, espressif/esp_io_expander_tca95xx_16bit and espressif/led_strip in `main/idf_component.yml`. Only speaker links them.

## Testing

- **Host:**
  - `test_soundlevel`: A-weighting response at 31.5 Hz..6.3 kHz within tolerance; a sine of known amplitude gives the right dBFS; LAeq of two levels is the energy mean; L90 from the histogram; gating excludes blocks; day periods split correctly at 07/19/23.
  - `test_ring`: fill and colour at the band edges, glide convergence, night dimming.
- **Board:**
  - The boot log shows the codec and expander up, EXIO8 LOW and EXIO6 untouched.
  - A 1 Hz serial line of LAF/LAeq; clapping moves LAF and the ring.
  - SD files appear once the clock is set.
  - The LED colour order and index 0's position are checked by eye.
