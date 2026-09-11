# wave — Waveshare ESP32-S3-LCD-1.47B

The fourth board: [Waveshare ESP32-S3-LCD-1.47B](https://www.waveshare.com/wiki/ESP32-S3-LCD-1.47B)
(ESP32-S3R8, 1.47" ST7789 172×320 over SPI, **with a QMI8658 soldered on**).

> **Verified on hardware 2026-09-11**, and it worked on the first flash:
> correct geometry, correct orientation, and the IMU answered at 0x6B.

## Why she exists

She is the board that can actually run the sand. lilly has no sensor of any
kind — her I²C bus scans empty — and the Feather's QMI8658 is a breakout that
would have to be unplugged from it, costing the Feather its spirit level. wave
has the same chip soldered to the board, so nothing moves and nothing is lost.

## Board comparison

| | Feather (`small`) | CrowPanel (`big`) | lilly | **wave** |
|---|---|---|---|---|
| Panel | ST7789 240×135 | 800×480 | ST7789 320×170 | ST7789 **320×172** |
| Bus | **SPI** | 16-bit RGB | 8-bit i80 | **SPI** |
| Layout | 20 × 5 | 64 × 20 | 26 × 7 | **26 × 7** |
| Flash / PSRAM | 4 / 2 MB | 4 / 8 MB | 16 / 8 MB | **16 / 8 MB** |
| IMU | breakout on QT | none | **none** | **onboard** |
| Pages | clock, message, sand, level | all of them | clock, message | clock, message, **sand** |

She shares the SPI backend with the Feather — same transport, same panel
driver — so `display_st7789.c` carries both, with the pins, size, gap and
clock chosen per board at the top of the file. Her 26 × 7 is identical to
lilly's, so the wrapping, the fonts and the sand's sizing all carry over
without a thought.

## Pin map

From Waveshare's own `Display_ST7789.h` and `I2C_Driver.h` in
`ESP32-S3-LCD-1.47B-Demo.zip` — their code, not a datasheet reading.

```
LCD (SPI)  MOSI 45   SCLK 40   CS 42   DC 41   RST 39   BL 46
           80 MHz, Offset_X 34, Offset_Y 0
I2C        SDA 48    SCL 47    400 kHz    QMI8658 at 0x6A/0x6B
RGB LED    38        BOOT button 0        battery ADC 1
TF card    CMD 15  SCK 14  D0 16  D1 18  D2 17  D3 21
```

**There is no switched panel rail.** Unlike the Feather's GPIO21 and lilly's
GPIO15, the panel is powered whenever the board is, so `display_st7789.c`
compiles that step out for her (`#undef PIN_TFT_POWER`). If a future board
needs one, define the pin and it comes back.

**The gap is Waveshare's number, not a guess.** They give `Offset_X 34` for
the panel upright; turned landscape the axes swap, so it becomes
`set_gap(0, 34)` — and (240 − 172) / 2 = 34 arrives at the same value
independently, which is the check worth doing.

## The sand

`PAGE_PARTICLES` is 323 grains — `particles_for(320, 172)`, one per 170 px² —
at a gravity of 1147 px/s², which is `GRAVITY_PER_ROW × 172`. Both are derived
from the panel rather than written down, so they are the Feather's tuning
carried across rather than a second set of magic numbers.

Confirmed on the panel at boot:

```
I (315) display: ST7789 up: 320x172, 26 cols x 7 rows
I (315) i2cbus:  bus up on SDA 48 / SCL 47
I (315) qmi8658: QMI8658 at 0x6B, revision 0x7C
I (395) main:    sand: 323 grains on 320x172, gravity 1147, seed 0xBE4CFFF9
```

**The axis signs will want setting once.** How the chip sits relative to the
panel decides them, and no still reading can reveal them, because gravity has
no component along an axis that is level. Send `!flip x`, `!flip y` or
`!flip swap` until the grains fall downhill; it persists in NVS. The sand also
negates one component against the shared mapping — a bubble floats against
gravity and grains fall with it — which is in `draw_particles()`, not in the
mapping, so correcting one page cannot break the other.

## Building and flashing

```sh
. tools/idf-env.sh
idf.py -B build-wave \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wave" \
  -D SDKCONFIG=sdkconfig.wave build

tools/flash-wave.sh --full      # first flash: bootloader + table + app
tools/flash-wave.sh             # after that: app only
```

Native USB-Serial-JTAG, so she behaves like the Feather and lilly: **never
pass `--baud 921600`**, and DTR/RTS software reset does not work (esptool
resets her fine on its own).

To see a boot log, hold the port and pulse RTS with DTR de-asserted. Driving
DTR during the reset drops her into `waiting for download` instead, because
DTR is IO0.

## Recovery

`firmware-backup/wave-stock-0x0-0x310000.bin` is her bootloader, partition
table and `app0` as shipped — an Arduino build, `idf v5.1.4`, June 2024. Her
stock table is `docs/hardware/wave-fingerprint.md`. `fingerprint.sh` returned
an empty partition table on this board, having caught it in the ROM
downloader; the table there was read separately and pasted in.

## Not implemented

The RGB LED (GPIO38), the TF slot, the battery ADC (GPIO1), and the BOOT
button. `buttons.c` wants two buttons for previous and next and she has one,
so she is not compiled with it; a single button would have to mean something
different. Nothing here needs any of them, and unused drivers make first
bring-up harder to debug.
