# Porting to the CrowPanel 7.0" HMI

Support for the [Elecrow CrowPanel 7.0" HMI](https://www.elecrow.com/esp32-display-7-inch-hmi-display-rgb-tft-lcd-touch-screen-support-lvgl.html)
(800×480, ESP32-S3-WROOM-1-N4R8).

> **Verified on hardware 2026-09-06.** Written before the board arrived, from
> Elecrow's published pin map and timings, and it worked on the first flash:
> correct geometry, no orientation or offset problems. An RGB panel has no
> MADCTL, so none of the mirror/gap trouble from the Feather applies.

## Board comparison

| | Feather ESP32-S3 TFT | CrowPanel 7.0 HMI |
|---|---|---|
| Chip | ESP32-S3, 4 MB flash, 2 MB PSRAM | ESP32-S3-WROOM-1-N4R8, 4 MB flash, **8 MB PSRAM** |
| Panel | ST7789 240×135 over **SPI** | 800×480 over **16-bit RGB parallel** |
| Framebuffer | 63 KB, fits SRAM | **750 KB, requires PSRAM** |
| Text layout | 20 × 5 | **64 × 20** |
| Clock font | 12×24 magnified | **dedicated 96×160 table** |
| Advertised name | `ESP32-Screen` | `peppa` |
| USB | Native USB-Serial-JTAG | **CH340 bridge on UART0** |
| Console | `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` | `CONFIG_ESP_CONSOLE_UART_DEFAULT` |
| Auto-reset | **Does not work** (no bridge) | **Works** (CH340 drives EN/IO0) |
| `--baud 921600` | Breaks the port | **Also breaks** — see below |
| Port | `/dev/cu.usbmodem*` | `/dev/cu.wchusbserial*` |
| Recovery | TinyUF2, double-tap RESET | None — flash the bootloader too |

Note how many of these are *inverted*. Habits from one board will mislead you on
the other.

## What changed in the code

Nothing above the panel. `ble_uart`, `textwrap`, `timecalc`, `main`, and the
`tell` client are untouched and shared.

Rendering moved out of the display driver into **`canvas.c`** — a framebuffer
plus glyph drawing that knows nothing about panels. Both boards use it, and
because it is pure memory manipulation it is now covered by host tests
(`host_tests/test_canvas.c`), which is how the 20×5 and 33×10 geometries are
confirmed without hardware.

That leaves each display backend responsible only for transport:

| File | Board |
|---|---|
| `display_st7789.c` | Feather — SPI bring-up, `esp_lcd_panel_draw_bitmap` |
| `display_rgb.c` | CrowPanel — RGB panel config, same blit call |

Selected by `CONFIG_SCREEN_BOARD_*` in `main/Kconfig.projbuild`.

## Panel configuration

From Elecrow's LovyanGFX setup in
[their repo](https://github.com/Elecrow-RD/CrowPanel-7.0-HMI-ESP32-Display-800x480).
**Identical across their V1.0, V2.0 and V3.0 revisions**, so the revision you
receive should not matter — worth re-checking against the board anyway.

```
data  B0-B4  15, 7, 6, 5, 4
      G0-G5  9, 46, 3, 8, 16, 1
      R0-R4  14, 21, 47, 48, 45
DE 41   VSYNC 40   HSYNC 39   PCLK 0   BACKLIGHT 2

pclk 15 MHz, pclk_active_neg
hsync  pulse 48, back porch 40, front porch 40
vsync  pulse 31, back porch 13, front porch  1
```

## Building and flashing

```sh
. tools/idf-env.sh
idf.py -B build-crowpanel \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.crowpanel" \
  -D SDKCONFIG=sdkconfig.crowpanel build
```

Unlike the Feather, **flash everything** — there is no TinyUF2 to preserve, so
the bootloader and partition table must be written:

```sh
esptool.py --chip esp32s3 --port /dev/cu.wchusbserial* write_flash \
  0x0     build-crowpanel/bootloader/bootloader.bin \
  0x8000  build-crowpanel/partition_table/partition-table.bin \
  0x10000 build-crowpanel/screen.bin
```

macOS 11+ ships a CH34x driver, so the port appears without installing anything.

**Use the default baud rate.** Measured on this board:

| Baud | Result |
|---|---|
| 921600 | `Changed.` then `No serial data received` |
| 460800 | `Invalid head of packet (0x05)` |
| 115200 | Works, but drops bytes on long continuous reads |

An earlier version of this document claimed high rates were fine here because
CH340 is a real UART bridge, unlike the Feather's USB-Serial-JTAG. That was
wrong. Both boards want the default rate, for different reasons.

**The serial console is unreliable on this board.** Bootloader output starts
clean and then garbles a few lines in, reproducibly. It is a link problem, not
a firmware one — the same CH340 corrupted a 4 MB flash read at 93%. Dump flash
in chunks with retries rather than in one pass, and do not read too much into a
garbled boot log: the firmware ran correctly the whole time it looked broken.

## First-flash checklist

1. Run `tools/fingerprint.sh` **before** flashing, and keep the output. It
   records the stock partition table and app, which is the only way back to
   the factory firmware.
2. Confirm the chip really is an S3 with 8 MB PSRAM. If PSRAM is missing or
   quad rather than octal, `display_init` fails on the framebuffer allocation
   and says so.
3. Watch the console at 115200 for `RGB panel up: 800x480, 33 cols x 10 rows`.
   Reaching that line means the panel accepted its configuration.

## What went wrong, and what did not

None of the display risks materialised. No tearing, no offset, no rotation
problem. The bounce buffer at `LCD_W * 10` and the 15 MHz pixel clock both work
as configured.

The one firmware bug was invisible: `font.h` selected its table with
`#if defined(CONFIG_SCREEN_BOARD_CROWPANEL_7)` without including `sdkconfig.h`,
which ESP-IDF does not force-include. The test was silently false, so the wrong
font compiled in and everything still ran perfectly. It was caught only because
the binary did not change size when a 9 KB larger table should have been added.
**Check that the image size moves when you expect it to.**

Still worth watching if the display is ever driven harder than static text:
a single framebuffer is written while the panel streams from it, so animation
may need `num_fbs = 2` and drawing to the back buffer.

## Not implemented

The capacitive touch panel and the SD slot. Nothing in this project needs them,
and adding an unused driver to unverified code would only make first bring-up
harder to debug.
