# Board fingerprint

> The partition table below was read separately with esptool and
> gen_esp32part.py: fingerprint.sh returned it empty on this board, having
> caught it in the ROM downloader rather than running its app.

Captured 2026-09-11 18:38 EDT from `/dev/cu.usbmodem1101`.

## USB identity

```
"idVendor" = 12346
"USB Product Name" = "USB JTAG_serial debug unit"
"USB Serial Number" = "44:1B:F6:86:54:1C"
"USB Vendor Name" = "Espressif"
```

## Chip

```
Detecting chip type... ESP32-S3
Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
Crystal is 40MHz
USB mode: USB-Serial/JTAG
MAC: 44:1b:f6:86:54:1c
Manufacturer: 20
Device: 4018
Detected flash size: 16MB
Flash type set in eFuse: quad (4 data lines)
Flash voltage set by eFuse to 3.3V
```

## eFuses

```
PSRAM_CAP (BLOCK1) PSRAM capacity = 8M R/W (0b01)
PSRAM_TEMP (BLOCK1) PSRAM temperature = 85C R/W (0b10)
PSRAM_VENDOR (BLOCK1) PSRAM vendor = AP_3v3 R/W (0b01)
PSRAM_CAP_3 (BLOCK1) PSRAM capacity bit 3 = False R/W (0b0)
FLASH_TPUW (BLOCK0) Configures flash waiting time after power-up; in u = 0 R/W (0x0)
FLASH_ECC_MODE (BLOCK0) Flash ECC mode in ROM = 16to18 byte R/W (0b0)
FLASH_TYPE (BLOCK0) SPI flash type = 4 data lines R/W (0b0)
FLASH_PAGE_SIZE (BLOCK0) Set Flash page size = 0 R/W (0b00)
FLASH_ECC_EN (BLOCK0) Set 1 to enable ECC for flash boot = False R/W (0b0)
FLASH_CAP (BLOCK1) Flash capacity = None R/W (0b000)
FLASH_TEMP (BLOCK1) Flash temperature = None R/W (0b00)
FLASH_VENDOR (BLOCK1) Flash vendor = None R/W (0b000)
DISABLE_WAFER_VERSION_MAJOR (BLOCK0) Disables check of wafer version major = False R/W (0b0)
WAFER_VERSION_MINOR_LO (BLOCK1) WAFER_VERSION_MINOR least significant bits = 2 R/W (0b010)
PKG_VERSION (BLOCK1) Package version = 0 R/W (0b000)
WAFER_VERSION_MINOR_HI (BLOCK1) WAFER_VERSION_MINOR most significant bit = False R/W (0b0)
WAFER_VERSION_MAJOR (BLOCK1) WAFER_VERSION_MAJOR = 0 R/W (0b00)
WAFER_VERSION_MINOR (BLOCK0) calc WAFER VERSION MINOR = WAFER_VERSION_MINOR_HI = 2 R/W (0x2)
 << 3 + WAFER_VERSION_MINOR_LO (read only) 
PSRAM_CAPACITY (BLOCK0) calc as = PSRAM_CAP_3 << 2 + PSRAM_CAP (read only) = 1 R/W (0b001)
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
app0,app,ota_0,0x10000,3M,
app1,app,ota_1,0x310000,3M,
ffat,data,fat,0x610000,10112K,
coredump,data,coredump,0xff0000,64K,
```

## Running application

```
project      arduino-lib-builder
version      6683a0d
built        Jun 24 2024 13:24:30
idf version  v5.1.4-358-gbd2b9390ef-dirty
elf sha256   5a2709a77232ea0ef5d6958696892f26cd0c89e54c4130349fefbc7a33a5c58e
```
