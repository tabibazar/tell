# lilly — LilyGO T-Display-S3

The third board: [LilyGO T-Display-S3](https://www.lilygo.cc/products/t-display-s3)
(ESP32-S3R8, 1.9" ST7789 320×170 over an 8-bit i80 parallel bus).

> **Not yet verified on hardware.** Written from LilyGO's published pin map.
> The panel offsets in particular are derived, not measured — see below.

## Board comparison

| | Feather (`small`) | CrowPanel (`big`) | **T-Display-S3 (`lilly`)** |
|---|---|---|---|
| Chip | ESP32-S3, 4 MB / 2 MB | ESP32-S3-WROOM-1-N4R8, 4 MB / 8 MB | ESP32-S3R8, **16 MB / 8 MB OPI** |
| Panel | ST7789 240×135, **SPI** | 800×480, **16-bit RGB** | ST7789 320×170, **8-bit i80** |
| Framebuffer | 63 KB, SRAM | 750 KB, needs PSRAM | **106 KB, SRAM** |
| Text layout | 20 × 5 | 64 × 20 | **26 × 7** |
| Clock font | 12×24 magnified | dedicated 96×160 | **12×24 magnified** |
| USB | USB-Serial-JTAG | CH340 on UART0 | **USB-Serial-JTAG** |
| Auto-reset | Does not work | Works | **Does not work** |
| Port | `/dev/cu.usbmodem*` | `/dev/cu.wchusbserial*` | `/dev/cu.usbmodem*` |
| Navigation | none | touch | **two buttons** |
| IMU pages | sand + spirit level | none | **sand** |
| Recovery | TinyUF2 | none | none |

lilly sits with the Feather on almost everything that is not the panel: native
USB, no auto-reset, the same 12×24 cell. The CrowPanel is the odd one out.

## Pin map

From LilyGO's
[`examples/factory/pin_config.h`](https://github.com/Xinyuan-LilyGO/T-Display-S3/blob/main/examples/factory/pin_config.h).

```
data  D0-D7  39, 40, 41, 42, 45, 46, 47, 48
WR 8   RD 9   DC 7   CS 6   RST 5   BACKLIGHT 38
PWR_ON 15
I2C   SDA 18   SCL 17
buttons  0 (BOOT), 14        battery ADC 4
pclk 16 MHz
```

**GPIO15 gates the peripheral rail.** Drive it high before anything else or the
panel never lights and the board looks dead rather than misconfigured. This is
the same class of mistake as the Feather's `PIN_TFT_POWER`, and it is the first
thing to check if a blank screen appears.

**GPIO9 (RD) must idle high.** It is unused — we only ever write — but left
floating the panel can see a read strobe.

## What changed in the code

Nothing above the panel, again. `canvas.c` does the rendering for all three
boards and the 26×7 geometry is covered by `host_tests/test_canvas.c`, so the
layout was confirmed before the board was ever flashed.

| File | Board |
|---|---|
| `display_st7789.c` | Feather — SPI |
| `display_rgb.c` | CrowPanel — RGB |
| `display_i80.c` | **lilly — i80 bus, same `esp_lcd_panel_draw_bitmap` blit** |

New: `buttons.c` / `buttons.h`, compiled only for this board.

## Pages: there are three

Pages are gated by the `everywhere` flag in `pagedefs.c`, which means "not only
the big panel". 26×7 will not hold views laid out for 64×20, so lilly is not
`big`. That leaves **Clock, Message and Sand** — the buttons cycle them and dismiss
the screensaver.

If lilly should show more, the question to answer first is which data views
degrade honestly at 26 columns.

## The sand, and the sensor it needs

`PAGE_PARTICLES` is a bottle of 320 grains (190 on the Feather) that pour
towards whichever way the board is tilted, with pairwise separation so a column of them holds itself up.
The physics is `main/particles.c` — no sensors, no panels, host-tested — and
`draw_particles()` in `main.c` is only the wiring.

**It needs the QMI8658, which lilly does not have.** The IMU is a breakout on
an I²C bus, not a part soldered to any board here, so it lives wherever it is
plugged in: the Feather's STEMMA QT (42/41) or lilly's bus (18/17). Only one
board can have it at a time, and **the Feather loses both its sand and its
spirit level while it is on lilly.** The same page runs on both; the sizes
below are what make one implementation fit either panel.

**The sand negates one gravity component and the level does not.** They share
`gravity_from()`, and a spirit level's bubble floats *against* gravity while
grains fall *with* it, so one of them has to invert. This was found on
hardware on 2026-09-10 — the bubble read correctly and the sand poured uphill.
It is in `draw_particles()` as `gy = -gy`, not in the shared mapping, so
correcting one page cannot break the other.

If no IMU answers at boot, `qmi8658_init()` returns `ESP_ERR_NOT_FOUND`, main
clears `PAGE_BIT(PAGE_PARTICLES)`, and the page is simply never offered. The
board does not fail; it just has two pages instead of three.

**The axis mapping is not a constant, and must not be guessed.** How the
breakout sits relative to lilly's panel depends on how it is mounted, and no
still reading can tell you the signs — gravity has no component along an axis
that is level. Send `!flip x`, `!flip y` or `!flip swap` and the board changes
them and remembers in NVS. Expect to use these on first run; they are why they
exist.

**Pull-ups are not a worry.** `i2cbus.c` enables the ESP32's internal ones, so
the bare header pins at 18/17 work without the Feather's STEMMA QT pull-ups.
They are weak (tens of kilohms), which is fine for short leads; long ones at
400 kHz would want real resistors.

Sizes scale with the panel rather than being absolute, so the bottle behaves
the same on either board: grain count is `particles_for(w, h)` at one grain per
170 px² (190 on the Feather, 320 on lilly), and gravity is per panel row, since
a taller bottle needs proportionally stronger gravity to fall through it in the
same time or it reads as smoke.

The bucket grid is sized to **exactly** 320×170. `particles_init` clamps rather
than growing, so a panel wider than lilly's needs `PARTICLES_GRID_W/H` raised,
not just a bigger `n` — a clamped grid folds the far columns together and
grains there quietly stop separating.

## Building and flashing

```sh
. tools/idf-env.sh
idf.py -B build-lilly \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.lilly" \
  -D SDKCONFIG=sdkconfig.lilly build

tools/flash-lilly.sh --full      # first flash: bootloader + table + app
tools/flash-lilly.sh             # after that: app only
```

**The first flash must be `--full`.** There is no TinyUF2 to preserve, and the
partition table is ours (`partitions-lilly.csv`, 16 MB, single app), so it has
to be written once.

**Never pass `--baud 921600`** — USB-Serial-JTAG ignores it and the port drops
with `Device not configured`, exactly as on the Feather.

## First-flash checklist

1. Run `tools/fingerprint.sh` **before** flashing, and keep the output. It is
   the only way back to LilyGO's factory firmware.
2. Watch the console for `i80 ST7789 up: 320x170, 26 cols x 7 rows`. Reaching
   that line means the panel accepted its configuration.
3. Check the image size moves when you expect it to — the CrowPanel port hid a
   wrong-font bug for a whole session because it did not.
4. The framebuffer is a single 106 KB DMA allocation. It fails loudly if it
   cannot be met (`no DMA memory for framebuffer`, then main halts), so a blank
   screen is not this — check GPIO15 first.

## Expect to correct the offsets

`display_i80.c` uses `swap_xy(true)`, `mirror(false, true)` and
`set_gap(0, 35)`. The 35 is arithmetic, not measurement: the ST7789 controller
has 240 rows of RAM and the panel shows 170 of them, centred, so (240−170)/2.
Community i80 configs for this board use the same value.

Colour is the other thing to watch, and the trap here is that it will look
fine. `display_i80.c` leaves `swap_color_bytes` off so the framebuffer goes out
in memory order, matching the Feather's SPI path. Monochrome text is
byte-symmetric — `0xFFFF` and `0x0000` survive any swap — so the clock and
messages prove nothing about it. Only a coloured element would.

The mirroring is the guess most likely to be wrong. The Feather's first attempt
came out upside down, and there is no way to tell from the datasheet which way
round LilyGO mounted the glass. If text appears mirrored or flipped, change
only the two booleans in `esp_lcd_panel_mirror`; if it is offset by a constant,
change the gap.

## Not implemented

The battery ADC (GPIO4), the SD slot, PSRAM, and the capacitive touch of the
`T-Display-S3-Touch` variant. Nothing here needs them, and unused drivers make
first bring-up harder to debug.
