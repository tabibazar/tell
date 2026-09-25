# Board fingerprint

Captured 2026-09-25 16:34 EDT from `/dev/cu.usbmodem101`.

## USB identity

```
"idVendor" = 12346
"USB Product Name" = "USB JTAG_serial debug unit"
"USB Serial Number" = "28:84:85:56:F6:B0"
"USB Vendor Name" = "Espressif"
```

## Chip

```
Detecting chip type... ESP32-S3
Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
Crystal is 40MHz
USB mode: USB-Serial/JTAG
MAC: 28:84:85:56:f6:b0
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
nvs,data,nvs,0x9000,16K,
otadata,data,ota,0xd000,8K,
phy_init,data,phy,0xf000,4K,
model,data,spiffs,0x10000,960K,
ota_0,app,ota_0,0x100000,6M,
ota_1,app,ota_1,0x700000,6M,
```

## Running application

```
no application image at this offset
```

## Factory firmware, from its boot log (2026-09-25)

xiaozhi (open-source AI voice assistant), partition layout nvs / otadata /
phy_init / `model` 960K (speech models) / ota_0 6M / ota_1 6M.
- Display: JD9853 over SPI (LVGL), backlight set to 75.
- Audio: "BoxAudioCodec" -- ES8311 (speaker DAC, slave) + ES7210 (4 mics,
  TDM), I2S at 24 kHz, Opus.
- Camera: cam_hal initialises, then "Detected camera not supported" (probe
  0x106): a sensor this build does not support, or none fitted.
- GPIO7 driven as an output early (likely a power/amp enable).
- Wi-Fi setup mode: softAP "Xiaozhi-F6B1", portal at http://192.168.4.1.
