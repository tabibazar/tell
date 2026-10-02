# spy: board fingerprint

Captured 2026-10-01 from the home Mac. Board: **Waveshare ESP32-S3-SIM7670G-4G**
(<https://www.waveshare.com/esp32-s3-sim7670g-4g.htm>), with an **OV5640** camera
fitted (not the OV2640 the listing ships).

## USB

One USB-C port feeds a **CH334** hub on the board, which carries two devices:

| Device | VID:PID | What it is |
|---|---|---|
| "USB Single Serial" (WCH **CH343**) | 1a86:55d3 | The ESP32-S3's UART0: flashing and the log. Its USB serial number is the CH343's own (`5B91031959`), **not** the board's MAC, so tools that find boards by USB serial number will not see spy by MAC; `tools/board-guard.sh` reads the MAC with esptool and is unaffected. |
| "QualComm Compo" | 05c6:9330 | The SIM7670G's own USB: four CDC serial ports (`cu.usbmodem…13/15/17/19`). The first answered AT commands. |

## Chip

```
Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
Crystal is 40MHz
MAC: 28:84:85:91:87:c4
Detected flash size: 16MB
```

8 MB PSRAM on this unit; Waveshare's page says the board carries an ESP32-S3R2 (2 MB).

## Modem (read-only AT queries)

```
Manufacturer: SIMCOM INCORPORATED
Model: SIM7670G-MNGV
Revision: V1.9.05   (+CGMR: 2374B01SIM767XM5A_M_CUS_WEIXUE)
+CPIN: READY
+CSQ: 25,0
+CEREG: 0,1
+COPS: 0,2,"302490",7          (Freedom Mobile, LTE)
+CPSI: LTE,Online,302-490,...,EUTRAN-BAND66,...
```

The modem registers on its own with a SIM in; the ESP32 does not need to start it.
(The IMEI is deliberately not recorded here: the repo is public.)

## Factory firmware

Backed up to `firmware-backup/sim7670g-factory-2884859187c4.bin` (git-ignored),
16 MB, sha256 `7c64948c750fcc85f9b7e56271c7680d2b40851e2d32b5003cfc4659e9090073`.

Partitions: nvs 0x9000, otadata 0xe000, app0 0x10000 (1.25 MB), app1 0x150000,
spiffs 0x290000, coredump 0x3f0000.

What it is: Waveshare's board test plus Espressif's CameraWebServer, Arduino-ESP32
3.3.5 (IDF v5.5.1), built 2025-12-18. In order it checks the MAX17048 fuel gauge,
starts the camera, mounts the microSD card (and stops there without one), then
brings up a Wi-Fi access point "ESP32-S3-A-SIM7670X-4G" (password 12345678) serving
the camera at http://192.168.4.1 (/stream, /capture, /control, /status). It does
not use the modem.

**Heat:** streaming from boot with the camera always powered, the OV5640 got hot
enough that Reza unplugged it within minutes. Our firmware should keep the camera
in power-down (PWDN) between uses, preview small and slow, and lower XCLK.
