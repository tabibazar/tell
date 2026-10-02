# spy (Waveshare ESP32-S3-SIM7670G-4G, V2.0): pinout research

Researched 2026-10-01 from the V2.0 schematic (read by its Altium net/pin text layer and by rendered crops), the V2 demo zip (Arduino and ESP-IDF), the V1 schematic and demo for comparison, Waveshare's docs pages, and SIMCom's hardware-design and AT manuals. Nothing here came from the board: no serial port was opened.

**Which revision spy is.** spy is a **V2.0** board. Three things point that way: the OV5640 camera (the FAQ says "new versions after 2026 come with the OV5640 by default, with a V2.0 silkscreen"), esptool's "Embedded PSRAM 8MB (AP_3v3)" (the V2 schematic's U3 is an **ESP32-S3R8**), and the factory build date of 2025-12-18 (the V2 schematic is dated 2025-12-15). The V1 board uses different pins for the camera, I2C, SD card-detect and modem power (see "V1 vs V2" below). Pins from V1 code, the V1 schematic, or the FAQ answer that says "GPIO33" do **not** apply to spy.

## gpio map

Sources:
- S = V2 schematic netlist plus crops.
- D = V2 demo code.
- W = docs.waveshare.com pages.

| GPIO | Function on spy | Sources | Notes |
|---|---|---|---|
| 0 | BOOT key (Key1), auto-program circuit; on P2 pin 15 | S | Strapping pin. Active low. |
| 1 | BAT_ADC through **R86 "NC/0R"**; on P2 pin 10 | S only | VBAT x 100k/(200k+100k) = Vbat/3, filtered by C94. It is not connected unless R86 is fitted, and the demos don't use it. |
| 2 | free; on P2 pin 8 | S | |
| 3 | free; on P2 pin 12 | S | Strapping pin (JTAG source select). |
| 4 | SD CMD (TF pin 3); on P3 pin 4 | S, D | The R38 pull-up is **NC**. |
| 5 | SD CLK (TF pin 5); on P3 pin 5 | S, D | The R39 pull-up is NC. |
| 6 | SD D0 (TF pin 7); on P3 pin 6 | S, D | The R41 pull-up is NC. |
| 7-14 | camera Y2-Y9 (D0-D7): 7=Y2, 8=Y3, 9=Y4, 10=Y5, 11=Y6, 12=Y7, 13=Y8, 14=Y9; on P3 pins 10-17 | S, D | Y0/Y1 (FPC pins 1/2) are unconnected, so the bus is 8-bit. |
| 15 | I2C SDA: camera SIO_DAT **and** MAX17048 SDA; on P3 pin 8 | S, D | Pulled up by R73 4.7k **to CV3V3 (the camera rail)**. |
| 16 | I2C SCL: camera SIO_CLK **and** MAX17048 SCL; on P3 pin 9 | S, D | Pulled up by R71 4.7k to CV3V3. |
| 17 | ESP **RX** <- modem TXD (through the U14 TXB0104 level shifter, R59 0R); on P2 pin 17 as "TXD1" | S, D | The demos use `RX_PIN 17` on UART_NUM_1 at 115200. |
| 18 | ESP **TX** -> modem RXD (through the TXB0104, R61 0R); on P2 pin 16 as "RXD1" | S, D | The demos use `TX_PIN 18`. |
| 19 / 20 | native USB D-/D+ -> R56/R57 0R -> FSUSB42 HSD1 -> **modem USB**; on P2 pins 13/14 | S, W | **Not** routed to the USB-C or the hub. See "USB". |
| 21 | **modem power enable** (gate of the FDC6333C VBAT->VVBAT switch, through R34 100R); on P2 pin 11 | S, D | The V2 GNSS demo does `pinMode(21, OUTPUT); digitalWrite(21, HIGH)`. HIGH = modem powered. Only effective with the "4G" DIP OFF. |
| 26-32 | 26 = SPICS1, the in-package PSRAM chip select; 27-32 = SPI flash (PY25Q128HA, 16 MB) | S | Do not use. |
| 33-37 | **octal PSRAM** (ESP32-S3R8) | S (U3 = ESP32-S3R8; pins unlabelled) | Do not use. |
| 38 | WS2812B-0807 RGB LED DIN (through R35 0R) | S, D | One LED, GRB order (`NEO_GRB` in the demo). |
| 39 | camera XCLK; on P2 pin 6 | S, D | Also MTCK. |
| 40 | modem **RI** (through the TXB0104 B4/A4); on P2 pin 5 | S only | Ring/URC indicator, active low. Input to the ESP32. |
| 41 | camera HREF; on P2 pin 4 | S, D | MTDI. |
| 42 | camera VSYNC; on P2 pin 9 | S, D | MTMS. |
| 43 / 44 | UART0 TX/RX -> CH343 (R52/R53 0R); on P2 pins 2/3 | S | Flashing and the log. |
| 45 | modem **DTR** (through the TXB0104 B3/A3); on P2 pin 7 | S only | **Strapping pin (VDD_SPI).** See traps. Output from the ESP32. |
| 46 | camera PCLK (C82 NC/15pF to GND); on P3 pin 7 | S, D | Strapping pin: download mode needs GPIO0 low **and** GPIO46 low, so a camera driving PCLK high at reset would break flashing. |
| 47 / 48 | unconnected, not on a header | S | Free, but unreachable without rework. |

Not available: GPIO22-25 do not exist on the S3, and there is no JTAG because 39-42 belong to the camera.

## camera (OV5640, 24-pin DVP FPC U13)

| Signal | GPIO | esp32-camera field |
|---|---|---|
| XCLK | 39 | pin_xclk |
| PCLK | 46 | pin_pclk |
| VSYNC | 42 | pin_vsync |
| HREF | 41 | pin_href |
| Y9..Y2 (D7..D0) | 14, 13, 12, 11, 10, 9, 8, 7 | pin_d7..pin_d0 |
| SIOD (SCCB SDA) | 15 | pin_sccb_sda |
| SIOC (SCCB SCL) | 16 | pin_sccb_scl |
| PWDN | **-1**: FPC pin 17 goes through R74 10k to GND, so the sensor is always enabled | pin_pwdn |
| RESET | **-1**: FPC pin 19 goes through R76 10k to 2V8, with C87 NC, so the sensor is always out of reset | pin_reset |

Sources: the schematic crop (U13 with every GPIO label, R74, R76) and V2 `examples/CameraWebServer/camera_pins.h`, section `CAMERA_MODEL_WAVESHARE_7670_BOARD`. They agree on every pin. The demo uses XCLK 20 MHz, JPEG, UXGA with `fb_count` 2 in PSRAM, then drops to QVGA.

Power:
- CV3V3 comes from buck channel 2 of an EA3036C (U11). Its EN2 is net **GVSET, the "CAM" DIP switch** (SW2-1, R60 47k pull-down; the switch ties it to VCC3V3 through D3).
- Two RT9166A LDOs make 2V8 (H5: AVDD and DOVDD) and 1V5 (H6: DVDD) from CV3V3.
- **No GPIO controls camera power, PWDN or RESET.** The FAQ agrees: CAM = "controls enabling/disabling the camera function; set to OFF to disable the camera".

What firmware can do about the heat (the OV5640 overheated under the factory demo):
1. **Software power-down over SCCB.** OV5640 register 0x3008 (SYSTEM_CTROL0): bit 7 = software reset, bit 6 = software power down (esp32-camera `ov5640_regs.h`). Write 0x42 to sleep and 0x02 to wake. SCCB stays alive while it sleeps. Check on the board that the sensor actually cools.
2. **Stop XCLK between captures.** `esp_camera_deinit()` stops the LEDC clock. Lower XCLK (for example 10 MHz) and keep the frame size small while previewing.
3. **Hardware option:** lift R74 and wire FPC pin 17 (PWDN) to free GPIO2 or GPIO3 for a real PWDN.
4. **Manual option:** the CAM DIP switch kills the whole rail. That also removes the I2C pull-ups (see traps).

## microSD (TF1, SDMMC 1-bit)

| TF pin | Signal | GPIO / net |
|---|---|---|
| 5 | CLK | 5 |
| 3 | CMD | 4 |
| 7 | D0 | 6 |
| 2 | CD/DAT3 | net SD_CS, **R37 10k pull-up to VCC3V3 only; no GPIO** |
| 8, 1 | D1, D2 | not connected |
| 9, 10 | card-detect switch | not connected |

- Sources: the schematic (TF1 crop) and the V2 `SD.ino` (`SD_MMC.setPins(5, 4, 6); SD_MMC.begin("/sdcard", true)`, i.e. 1-bit mode). The factory firmware strings show the IDF `sdmmc_host` driver, not SDSPI.
- **1-bit SDMMC is the only mode.** 4-bit is impossible because D1 and D2 are unwired. SPI mode is impossible because CS/D3 is hard-pulled high with no GPIO to pull it low.
- The CMD/CLK/D0 pull-ups (R38, R39, R41) are **not fitted**. Rely on the internal pull-ups, which SD_MMC and `sdmmc_slot_config_t` flag `SDMMC_SLOT_FLAG_INTERNAL_PULLUP` enable, and keep the clock modest (default 20 MHz) if mounting is flaky.
- There is no card-detect. V1 had CD on GPIO46; V2 dropped it.

## SIM7670G modem

The schematic symbol is "A7670E-FASE" (U7A). The A7670E and SIM7670G share a footprint, and Waveshare uses one board for both.

### UART (115200 8N1 by default)

The ESP32 side is 3.3 V and the modem side is 1.8 V, joined by a **TXB0104PWR** (U14). Its VCCA = VCC_1V8 = the modem's VDD_EXT; VCCB = VCC3V3; OE = VCC_1V8.

| Modem pin | Modem signal | TXB0104 A/B | ESP32 | Direction |
|---|---|---|---|---|
| 9 | TXD | A2/B2 (net TXD1) | **GPIO17** | modem -> ESP (ESP RX) |
| 10 | RXD | A1/B1 (net RXD1) | **GPIO18** | ESP -> modem (ESP TX) |
| 3 | DTR | A3/B3 | **GPIO45** | ESP -> modem (sleep control) |
| 4 | RI | A4/B4 | **GPIO40** | modem -> ESP |
| 7 / 8 | CTS / RTS | - | not wired | no hardware flow control |
| 5 | DCD | - | not wired | |
| 16 | RESET | - | **not wired** (the net label goes nowhere) | |
| 66 | STATUS | - | **not connected** | |
| 52 | NETLIGHT | - | LED3 (red) through R47 51R only | not readable by the ESP32 |

- Sources: the schematic (U14 crop, U7A crop, netlist) and the V2 ESP-IDF GNSS/TCP/HTTP examples (`RX_PIN 17`, `TX_PIN 18`, UART_NUM_1, 115200, `UART_HW_FLOWCTRL_DISABLE`). The Arduino GNSS demo uses SoftwareSerial(RX 17, TX 18) at 115200.
- When the modem is off, VCCA is 0 V, so the TXB0104 goes high-impedance on both sides. Expect GPIO17/40 to float. That is harmless, but don't read RI as meaningful then.
- W (NETLIGHT LED): "Network indicator: red, flashes with a 200 ms interval after network registration".

### Power, PWRKEY and reset

From the schematic crop (the M2/Q9 block):
- The modem's VBAT is net **VVBAT**, switched from VBAT by an FDC6333C (M2B P-FET, M2A N-FET driver). M2A's gate is driven by **BAT_SET** (the "4G" DIP, SW2-3, through R32 100R) **OR GPIO21** (through R34 100R), with R33 100k to GND. Either one high powers the modem.
- **PWRKEY is held low permanently** whenever VVBAT is present: Q9 MMBT3904's base is fed from VVBAT through R30 4.7k/R31 47k, and its collector goes to PWRKEY. This is SIMCom's reference PWRKEY circuit with the "pulse" input tied to VVBAT. So the module **auto-starts the moment VVBAT appears**. There is **no PWRKEY GPIO and no pulse to time**. This matches the fingerprint observation that the modem registered on its own.
- The modem's RESET pin is not wired. USB_BOOT (pin 6) is pulled down only through R77, which is NC.
- W (FAQ): "keep the DIP switch off and control it through GPIO... GPIO high: module powers on; GPIO low: module powers off." The FAQ names "GPIO33 or GPIO22". GPIO33 is the **V1** pin (the V1 netlist has GPIO33 -> R34), GPIO22 doesn't exist on an S3, and V2 moved this to **GPIO21**. Two sources for GPIO21 on V2: the schematic and the V2 GNSS .ino.

SIMCom timings (SIM7672X Series Hardware Design V1.01, the sheet Waveshare links for the SIM7670G):
- Supply and power-on: VBAT 3.4-4.2 V (3.8 V recommended). Power-on Ton 50 ms typical. STATUS high after 320 ms. **UART ready after 55 ms, USB ready after 470 ms** (budget a few seconds before AT is reliable).
- Power-off: PWRKEY low for at least 2.5 s, or `AT+CPOF`. Toff-on buffer at least 2 s. VBAT must fall below 1.3 V before the next power-on, otherwise the module restarts by itself.
- Reset: Treset 0.5 s (unused here: RESET is unwired).
- SIMCom: "the customer cannot shut down VBAT by disconnecting it, which may cause damage to flash". Send `AT+CPOF` first, wait, then drop GPIO21.

### USB, the CH334 hub and the FSUSB42 mux

Paths:
- The USB-C (H3) goes through R23/R25 22R to the **CH334F hub** upstream (DPU/DMU).
  - Hub port 4 goes to the **CH343P** UART bridge, then to the ESP32 UART0 (GPIO43/44). This is the flashing and log port, with DTR/RTS auto-program through Q4/Q7.
  - Hub port 3 (net USB3) goes to **FSUSB42UMX (U12) HSD2**.
  - Hub ports 1 and 2 are unused.
- The modem's USB D+/D- (pins 27/28, nets USB_P/USB_N) go to the FSUSB42 common D+/D- pins.
- The ESP32-S3 native USB (GPIO19/20, net USB2) goes to the **FSUSB42 HSD1**, and nowhere else apart from P2 pins 13/14.
- The FSUSB42 SEL input is net **USB_SET**: the "USB" DIP (SW2-4) with R65 47k pull-down. FSUSB42 truth table: SEL low gives D <-> HSD1, SEL high gives D <-> HSD2.
  - **USB DIP ON**: modem USB -> hub -> USB-C. The Mac sees "QualComm Compo" 05c6:9330.
  - **USB DIP OFF**: modem USB <-> ESP32 native USB, for the TinyUSB host + PPP 4G-router firmware.
- Second source for the polarity: Reza's Mac enumerated the modem's four CDC ports (spy-fingerprint.md), so the DIP is ON = SEL high = HSD2 = hub. This is consistent. W: "4G module USB <-> Type-C / 4G module USB <-> ESP32-S3", and the FAQ: "USB: set OFF, cannot access the module via USB, enables hotspot function".
- **The ESP32-S3's own USB never reaches the USB-C.** USB-Serial-JTAG and USB-CDC console are unavailable. Flash and log only through the CH343. W: "USB-to-UART chip with automatic download circuit".
- The hub and CH343 run on **UV3V3**: EA3036C channel 3, EN3. EN3 is drawn on the same wire as nets UVSET (the "HUB" DIP, SW2-2) and UVBUS. W: "HUB: controls power to the USB HUB circuit; set to OFF when using battery to enable low power mode".

### GNSS

- GNSS is **inside the SIM7670G**: the 1V8_GNSS/GNSS_TXD/GNSS_RXD pins are strapped inside the module (GNSS_TXD -> R58 1k -> UART3_RX, GNSS_RXD -> R54 1k -> UART3_TX, GNSS_PWRCTL -> R64 -> module pin 20). No ESP32 pin is involved.
- Antennas:
  - The GNSS antenna goes on its **own IPEX connector "GNSS1"** (net GNSS_ANT, module pin 90, through L10/C77/C80).
  - LTE goes on IPEX "Main1" (RF_ANT, pin 60).
  - W: "Onboard GNSS IPEX1 connector".
- AT sequence used by both V2 demos:
  1. `AT+CGNSSPWR=1`. Wait for it: the Arduino demo waits 10 s, and the IDF demo waits for `+CGNSSPWR: READY!`.
  2. `AT+CGNSSTST=1`.
  3. `AT+CGNSSPORTSWITCH=<parsed>,<raw>`. SIM767XX AT manual section 21.2.10: parsed 0 = USB AT port, 1 = UART; raw NMEA 0 = USB NMEA port, 1 = UART. The IDF demo sends `1,1` (everything to UART, i.e. GPIO17). The Arduino demo sends `0,1` (parsed to USB, raw NMEA to UART).
  4. Then `AT+CGPSINFO` or `AT+CGNSSINFO`.
- GNSS_VBKP (pin 116) goes straight to VCC3V3, and R42 0R joins 1V8_GNSS (pin 97) to VCC_1V8 (with C57 2.2 uF). So there is no separate backup cell, and hot start lasts only while 3V3 is up.

## MAX17048 fuel gauge (U10)

| Item | Value | Sources |
|---|---|---|
| I2C address | 0x36 (7-bit) | D (`MAX17048_I2C_ADDRESS 0x36`), part datasheet |
| SDA / SCL | GPIO15 / GPIO16, shared with camera SCCB | S, D (`Wire.begin(15, 16)` in bat.ino and the GNSS demo) |
| ALRT# | **tied to GND through R45 0R**: not usable, not on a GPIO | S (crop) |
| QSTRT | GND | S |
| VDD / CELL | net VBAT, the same net that feeds the EA3036C and the modem switch. Whether that is before or after SW1 was not traced. | S |

- Register 0x02 is VCELL (78.125 uV/LSB). Register 0x04 is SOC (1/256 %).
- The demo's `bat.ino` reads 0x02 and scales it by 5/65535, which gives **volts**, not the "%" its V1 print claimed.
- The factory firmware checks the gauge first at boot.

## RGB LED, keys and LEDs

- **RGB:** one WS2812B-0807 (LED_RGB1). DIN = GPIO38 through R35 0R; VDD = VCC3V3; DOUT unconnected. Sources: S, and D (`Adafruit_NeoPixel(1, 38, NEO_GRB + NEO_KHZ800)`); W: "RGB LED, driven by WS2812B".
- **Key1 = BOOT** on GPIO0 (to GND). **Key2 = RESET** on CHIP_PU (R14 10k, C25 1uF). There is **no user button**.
- Fixed LEDs:
  - Power: blue LED4, from VCC3V3 through R50 4.3k.
  - Network: red LED3, from modem NETLIGHT.
  - Solar charging: green, from the CN3791 CHRG output.
  - Battery reversed: yellow.
  - None of them reads back to the ESP32.

## power tree (for firmware decisions)

- **USB-C 5 V (UVBUS)** feeds the **ETA6098** charger (U9) for the 18650. **Solar** (5-6 V default; the R78-R84 jumpers select higher) feeds the **CN3791** MPPT charger (U1). Battery protection is an S-8261 + FS8205 pair. The 18650 holder has a slide switch, **SW1**.
- VBAT feeds **EA3036C** U11, a triple buck:
  - ch1 = VCC3V3, the ESP32 and the rest; EN1 is tied to VBAT, always on.
  - ch2 = CV3V3, the camera; EN2 = "CAM" DIP.
  - ch3 = UV3V3, the hub and CH343; EN3 = UVSET/UVBUS.
- An SY8105 buck (U8) is also on the sheet, from UVBUS.
- **The modem draws straight from VBAT (VVBAT)**, not a regulated rail. It browns out below 3.4 V, before the ESP32 does.
- **There is no firmware power-hold and no soft-off.** The only power controls firmware has are GPIO21 (modem, DIP OFF) and the CAM rail (DIP only).
- The DIP switch SW2 is 4-way. Each line has a 47k pull-down, and the switch pulls it up to VCC3V3 through D3 B5819WS:

| SW2 | Net | Controls | FAQ label |
|---|---|---|---|
| 1 | GVSET | EA3036C EN2: camera rail CV3V3 (and the I2C pull-ups) | CAM |
| 2 | UVSET | EA3036C EN3: hub + CH343 rail UV3V3 | HUB |
| 3 | BAT_SET | modem VBAT switch (OR'd with GPIO21) | 4G |
| 4 | USB_SET | FSUSB42 SEL: ON = modem USB to USB-C, OFF = modem USB to ESP32 | USB |

## headers

P2 ("UART" / "Free" on the silkscreen), pins 1-19:

| Pins | Signals |
|---|---|
| 1-6 | GND, U0TXD (GPIO43), U0RXD (GPIO44), GPIO41, GPIO40, GPIO39 |
| 7-12 | GPIO45, GPIO2, GPIO42, GPIO1, GPIO21, GPIO3 |
| 13-19 | GPIO19 (USB D-), GPIO20 (USB D+), GPIO0, RXD1 (GPIO18 side), TXD1 (GPIO17 side), VBAT, GND |

P3 ("TFCard" / "Camera"), pins 1-19:

| Pins | Signals |
|---|---|
| 1-6 | VCC3V3, VCC3V3, GND, GPIO4, GPIO5, GPIO6 |
| 7-12 | GPIO46, GPIO15, GPIO16, GPIO7, GPIO8, GPIO9 |
| 13-19 | GPIO10, GPIO11, GPIO12, GPIO13, GPIO14, UVBUS, GND |

Sources: the schematic header crop and the netlist; they agree.

## V1 vs V2 (why V1 code and the FAQ's GPIO33 are wrong for spy)

| Function | V1 (S3R2, OV2640) | V2 = spy (S3R8, OV5640) | V1 source | V2 source |
|---|---|---|---|---|
| Camera XCLK / HREF / VSYNC / PCLK | 34 / 35 / 36 / 37 | **39 / 41 / 42 / 46** | V1 netlist; V1 demo uses `CAMERA_MODEL_XIAO_ESP32S3` pins | V2 schematic; `CAMERA_MODEL_WAVESHARE_7670_BOARD` |
| Camera Y2-Y9, SCCB | 7-14, 15/16 | same | | |
| MAX17048 I2C SDA/SCL | 3 / 2 | **15 / 16** (shared with SCCB) | V1 `bat.ino` `Wire.begin(3, 2)` | V2 `Wire.begin(15, 16)` |
| SD card-detect | GPIO46 | none | V1 `SD.ino` `SD_CD_PIN 46` | V2 schematic: CD unwired |
| Modem power enable | GPIO33 | **GPIO21** | V1 GNSS demo `pinMode(33...)`; V1 netlist | V2 GNSS demo `pinMode(21...)`; V2 schematic |
| PSRAM | quad 2 MB (33-37 usable) | **octal 8 MB (33-37 reserved)** | | esptool on spy; V2 U3 = ESP32-S3R8 |

## conflicts and traps

1. **The silkscreen and FAQ "Free" bracket on P2 is wrong for V2.** It brackets GPIO41/40/39/45/2/42/1/21. On V2, 39/41/42 are camera, 40 = modem RI, 45 = modem DTR and 21 = modem power. Truly free on headers: **GPIO2, GPIO3**, and GPIO1 if R86 stays unfitted. Free with the camera unused: 7-14 and 39/41/42/46. Free with the SD card unused: 4/5/6.
2. **The I2C pull-ups live on the camera rail.** R71/R73 go to CV3V3. With the CAM DIP OFF the gauge's bus has no external pull-ups, and the unpowered OV5640 sits on SDA/SCL (its I/O clamp may drag the lines down). Expect MAX17048 reads to fail with CAM off unless the internal pull-ups are enabled, and even then it is unverified. Check on hardware.
3. **GPIO45 is the VDD_SPI strapping pin and is driven by TXB0104 B3 (modem DTR)** whenever the modem is powered (VCCA up). The TXB0104's roughly 4 kohm one-shot drivers can beat the S3's weak internal pull-down at reset. If the board ever refuses to boot (flash errors) with the modem powered, suspect this: power the modem off (DIP 4G OFF) and retry. It boots today, so DTR evidently sits low or the VDD_SPI eFuse is set; the eFuse state is unread. GPIO46 (PCLK) and GPIO0/3 are the other straps; GPIO46 must be low at reset, which holds while the camera has no XCLK.
4. **GPIO21 against the 4G DIP.** With the DIP ON, BAT_SET is pulled to about 3.0 V, and GPIO21 driven LOW sinks about 15 mA through R32 + R34 (200R) while the gate floats mid-rail. It would neither cleanly switch the modem off nor be good for either part. Leave GPIO21 as an input (Hi-Z) while the DIP controls power, and drive it only with the DIP OFF, which is what the FAQ says too.
5. **There is no clean, firmware-only modem power cycle.** PWRKEY is permanently low, so it can't be pulsed. `AT+CPOF` then GPIO21 LOW is the SIMCom-safe order. Wait at least 2 s, and for VBAT below 1.3 V, before GPIO21 HIGH again. Whether the module restarts by itself after `AT+CPOF` while PWRKEY stays low is **unknown**; check on hardware.
6. **The camera has no firmware power or PWDN control.** The demo's `PWDN_GPIO_NUM -1` is correct, not lazy. Use SCCB software standby (0x3008 bit 6) and stop XCLK, or rework R74 (see the camera section).
7. **Don't copy the demo's `LCD.ino`.** It drives SCLK 39, MOSI 41, CS 45, DC 42, RST 1 and **BL 21**: camera pins, modem DTR, and PWM on the modem-power pin. It is a leftover from another board.
8. **The FAQ's PSRAM advice conflicts with itself.** It says "PSRAM: QSPI PSRAM" in one answer and "OPI PSRAM" in another. spy reports 8 MB embedded PSRAM, which is the S3R8, so it is **OPI**: Arduino "OPI PSRAM", IDF `CONFIG_SPIRAM_MODE_OCT`. The V2 IDF demos' sdkconfig says 8 MB flash; the board has 16 MB.
9. **The native USB isn't reachable from the Mac** (see USB). Don't plan on USB-CDC console or USB-JTAG. GPIO19/20 are spoken for by the mux whenever the USB DIP is OFF, and dangle on P2 otherwise.
10. **On battery with HUB OFF and no USB**, the CH343 is unpowered: no log. The modem's 1.8 A bursts come straight off the 18650. The FAQ quotes about 1.8 A and 9 W with camera plus streaming.
11. **The factory demo's camera recipe is the heat recipe:** XCLK 20 MHz, UXGA frame buffers, continuous stream, camera rail always on.
12. **SD has no pull-ups and no card-detect** (R38/R39/R41 NC; CD unwired). Use internal pull-ups and treat "mount failed" as "no card".
13. **First power on battery only needs USB once.** The FAQ: "after installing the battery for the first time, connect external power to activate the protection" (the S-8261/FS8205 latch).

## confidence

HIGH (schematic and V2 code agree, crops read):
- Camera pins: 39/46/42/41, 7-14, 15/16. PWDN tied low and RESET tied high (crop).
- SD 1-bit on 5/4/6, D3 pull-up only.
- Modem UART 17 (RX) / 18 (TX).
- GPIO21 = modem power enable (the schematic crop plus the V2 GNSS demo).
- MAX17048 at 0x36 on 15/16, ALRT# grounded (crop).
- WS2812 on 38.
- BOOT/RESET keys.
- The DIP functions (schematic plus FAQ table).
- The USB topology: native USB -> mux -> modem only. The fingerprint confirms the SEL polarity.

MEDIUM-HIGH (schematic only, one source):
- DTR = GPIO45, RI = GPIO40 (TXB0104 crop).
- BAT_ADC on GPIO1 behind R86 NC.
- EN3's exact wiring (UVSET and UVBUS labels on one wire).
- The header pin numbering.

UNKNOWN / needs a hardware check:
- Gauge I2C with CAM OFF.
- Whether R86 is fitted on spy.
- The VDD_SPI eFuse state (the GPIO45 strap risk).
- Modem behaviour after `AT+CPOF` with PWRKEY held low.
- That OV5640 software standby actually stops the heating.
- Whether the factory firmware's exact pin numbers match. Its strings carry no pin information; only the use of SDMMC and the MAX17048-first order are visible.

## sources

- V2 schematic (dated 2025-12-15, "ESP32-S3-A-SIM7670X-4G V2.0"): https://files.waveshare.com/wiki/ESP32-S3-A-SIM7670X-4G-HAT/ESP32-S3-A-SIM7670X-4G-V2.pdf, saved in the scratchpad at `spy/` as `ESP32-S3-A-SIM7670X-4G-V2.pdf`.
  - Text layer: `v2sch.raw.txt`, `v2bbox.html`.
  - Parsed netlist: `v2nets.txt` (via `nets.py`, `u3map.py`, `comp.py`).
  - 900-dpi render: `p1-1.png`.
  - Crops: `crops/{pwr,cam,camrst,dip,buck,max,modem,mrst,mux,lvl,batadc,sdrgb,hdr}.png`.
- V1 schematic (2024-01-31): https://files.waveshare.com/wiki/ESP32-S3-A7670E-4G/ESP32-S3-A-SIM7670X-4G.pdf, saved in the scratchpad as `v1nets.txt`.
- V2 demo zip: https://files.waveshare.com/wiki/ESP32-S3-A-SIM7670X-4G-HAT/Demo/ESP32-S3-A-SIM7670X-4G-V2.zip, unpacked under `spy/v2/ESP32-S3-A-SIM7670X-4G-V2/`.
  - Arduino-V3.3.4 examples: `CameraWebServer/{camera_pins.h, board_config.h, CameraWebServer.ino}`, `SD/SD.ino`, `RGB/RGB.ino`, `bat/bat.ino`, `GNSS-With-WaveshareCloud/*.ino`, `LCD/LCD.ino`.
  - `ESP-IDF/ESP32-S3-XXX7670X-4G-{GNSS,TCP,HTTP}/main/*.c`.
- V1 demo zip: https://files.waveshare.com/wiki/ESP32-S3-A7670E-4G/ESP32-S3-A-SIM7670X_4G.zip, unpacked to `spy/v1/` (camera_pins.h XIAO pins, `bat.ino` `Wire.begin(3,2)`, `SD.ino` CD 46, GNSS demo `pinMode(33)`).
- Docs: https://docs.waveshare.com/ESP32-S3-SIM7670G-4G and its `/FAQ`, `/Arduino`, `/ESP-IDF`, `/Resources-And-Documents` and `/Firmware` pages. The old wiki URL https://www.waveshare.com/wiki/ESP32-S3-SIM7670G-4G redirects there. Text saved as `spy/docs*.txt`.
  - The FAQ gives the DIP table, the GPIO33/22 power answer, the PSRAM settings and "V2.0 = OV5640".
- SIM7672X Series Hardware Design V1.01: https://files.waveshare.com/wiki/SIM7670G-LTE-Cat-1-GNSS-HAT/SIM7672X_Series_Hardware_Design_V1.01.pdf, saved as `spy/hd.txt` (PWRKEY reference circuit, Ton/Toff, Treset, VBAT 3.4-4.2 V, the 1.3 V restart note).
- SIM767XX Series AT Command Manual V1.01: https://files.waveshare.com/wiki/SIM7670G-LTE-Cat-1-GNSS-HAT/SIM767XX_Series_AT_Command_Manual_V1.01.pdf, saved as `spy/at.txt` (section 21.2.10 AT+CGNSSPORTSWITCH parameter meanings).
- esp32-camera `sensors/private_include/ov5640_regs.h`: https://github.com/espressif/esp32-camera (SYSTEM_CTROL0 0x3008: bit 7 reset, bit 6 software power down).
- Factory firmware: `firmware-backup/sim7670g-factory-2884859187c4.bin`, read-only `strings` (SDMMC host driver present; no pin information).
- docs/hardware/spy-fingerprint.md: the modem's USB enumerated on the Mac, which is the second source for the FSUSB42 SEL polarity; 8 MB PSRAM.

The scratchpad is `/private/tmp/claude-501/-Users-reza-PycharmProjects-esp32-small/1b836f61-b9e9-4742-b0ec-32bbf50008b5/scratchpad/spy/`.
