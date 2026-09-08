# Feather ESP32-S3 TFT — fingerprint

Reference profile of the board this project was developed against, so a second
board can be compared against it field by field.

Regenerate with `tools/fingerprint.sh [port] > docs/hardware/<board>.md`.
It is read-only — it reads identity, eFuses, flash and the partition table, and
resets the board back into its application when it finishes.

## Facts the probe cannot see

| | |
|---|---|
| Board | Adafruit Feather ESP32-S3 TFT |
| Display | ST7789, 240×135, 1.14" IPS, SPI |
| Panel offset | `set_gap(40, 53)` with `swap_xy(true)`, `mirror(true, false)` |
| Display pins | POWER 21, CS 7, DC 39, RST 40, BACKLITE 45, SCK 36, MOSI 35 |
| Text layout | 12×24 font → 20 columns × 5 rows |
| Bootloader | Adafruit TinyUF2 in the `factory` slot; double-tap RESET for recovery |
| I2C bus | SDA 42, SCL 41 (STEMMA QT), powered from GPIO21 with the panel |
| IMU | QMI8658 at `0x6B`, WHO_AM_I `0x05`, revision `0x7B` |
| Also on the bus | Something at `0x77`, a BMP280/BME280 family part. Unused. |
| IMU orientation | Z is normal to the board; X and Y lie in the panel's plane |

## What to compare on a new board

These are the fields that actually change what the firmware must do:

- **Chip and revision** — an S3 versus a C3/C6/P4 changes the BLE stack config and
  whether native USB exists at all.
- **Flash and PSRAM size** — decides whether `ota_0` is large enough, and whether
  a bigger framebuffer can live in SRAM or needs PSRAM.
- **Partition table** — the app offset is `0x10000` *here*; do not assume it.
- **USB mode** — `USB-Serial/JTAG` means no bridge chip, so DTR/RTS reset does
  not work and `--baud 921600` breaks the port.
- **Display controller and resolution** — a larger panel means a different
  driver, different `set_gap`, and a different rows/columns budget.

A larger display is the interesting one: `display.c` hardcodes `LCD_W`/`LCD_H`
and `DISPLAY_COLS`/`DISPLAY_ROWS`, with static assertions tying them to the font
cell. Those assertions will fail loudly rather than silently misrender, which is
the intended behaviour.

---


Captured 2026-09-06 12:02 EDT from `/dev/cu.usbmodem21201`.

## USB identity

```
"idProduct" = 4097
"idVendor" = 12346
"USB Product Name" = "USB JTAG_serial debug unit"
"USB Serial Number" = "68:EE:8F:DA:62:28"
"USB Vendor Name" = "Espressif"
```

## Chip

```
Detecting chip type... ESP32-S3
Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded Flash 4MB (XMC), Embedded PSRAM 2MB (AP_3v3)
Crystal is 40MHz
USB mode: USB-Serial/JTAG
MAC: 68:ee:8f:da:62:28
Manufacturer: 46
Device: 4016
Detected flash size: 4MB
Flash type set in eFuse: quad (4 data lines)
Flash voltage set by eFuse to 3.3V
```

## eFuses

```
PSRAM_CAP (BLOCK1) PSRAM capacity = 2M R/W (0b10)
PSRAM_TEMP (BLOCK1) PSRAM temperature = 105C R/W (0b01)
PSRAM_VENDOR (BLOCK1) PSRAM vendor = AP_3v3 R/W (0b01)
PSRAM_CAP_3 (BLOCK1) PSRAM capacity bit 3 = False R/W (0b0)
FLASH_TPUW (BLOCK0) Configures flash waiting time after power-up; in u = 0 R/W (0x0)
FLASH_ECC_MODE (BLOCK0) Flash ECC mode in ROM = 16to18 byte R/W (0b0)
FLASH_TYPE (BLOCK0) SPI flash type = 4 data lines R/W (0b0)
FLASH_PAGE_SIZE (BLOCK0) Set Flash page size = 0 R/W (0b00)
FLASH_ECC_EN (BLOCK0) Set 1 to enable ECC for flash boot = False R/W (0b0)
FLASH_CAP (BLOCK1) Flash capacity = 4M R/W (0b010)
FLASH_TEMP (BLOCK1) Flash temperature = 105C R/W (0b01)
FLASH_VENDOR (BLOCK1) Flash vendor = XMC R/W (0b001)
DISABLE_WAFER_VERSION_MAJOR (BLOCK0) Disables check of wafer version major = False R/W (0b0)
WAFER_VERSION_MINOR_LO (BLOCK1) WAFER_VERSION_MINOR least significant bits = 2 R/W (0b010)
PKG_VERSION (BLOCK1) Package version = 0 R/W (0b000)
WAFER_VERSION_MINOR_HI (BLOCK1) WAFER_VERSION_MINOR most significant bit = False R/W (0b0)
WAFER_VERSION_MAJOR (BLOCK1) WAFER_VERSION_MAJOR = 0 R/W (0b00)
WAFER_VERSION_MINOR (BLOCK0) calc WAFER VERSION MINOR = WAFER_VERSION_MINOR_HI = 2 R/W (0x2)
 << 3 + WAFER_VERSION_MINOR_LO (read only) 
PSRAM_CAPACITY (BLOCK0) calc as = PSRAM_CAP_3 << 2 + PSRAM_CAP (read only) = 2 R/W (0b010)
 HMAC module 
MAC (BLOCK1) MAC address 
CUSTOM_MAC (BLOCK3) Custom MAC 
DIS_DOWNLOAD_ICACHE (BLOCK0) Set this bit to disable Icache in download mode (b = False R/W (0b0)
DIS_DOWNLOAD_DCACHE (BLOCK0) Set this bit to disable Dcache in download mode ( = False R/W (0b0)
DIS_DOWNLOAD_MANUAL_ENCRYPT (BLOCK0) Set this bit to disable flash encryption when in d = False R/W (0b0)
SPI_BOOT_CRYPT_CNT (BLOCK0) Enables flash encryption when 1 or 3 bits are set = Disable R/W (0b000)
SECURE_BOOT_EN (BLOCK0) Set this bit to enable secure boot = False R/W (0b0)
DIS_DOWNLOAD_MODE (BLOCK0) Set this bit to disable download mode (boot_mode[3 = False R/W (0b0)
DIS_USB_OTG (BLOCK0) Set this bit to disable USB function = False R/W (0b0)
```

## Partition table

```
nvs,data,nvs,0x9000,20K,
otadata,data,ota,0xe000,8K,
ota_0,app,ota_0,0x10000,1408K,
ota_1,app,ota_1,0x170000,1408K,
uf2,app,factory,0x2d0000,256K,
ffat,data,fat,0x310000,960K,
```

## Running application

```
project      screen
version      3bfc4ed-dirty
built        Sep  6 2026 08:45:02
idf version  v5.5.5
elf sha256   20d0c4b6d948889028ccfcd1b9a6e756abda74e65cdd5b743dcd76f8911500c0
```
