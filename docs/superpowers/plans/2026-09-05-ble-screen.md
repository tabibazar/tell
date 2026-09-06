# BLE Screen Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Send text from a Mac to the Feather ESP32-S3 TFT over BLE and render it, wrapped, on the built-in ST7789 panel.

**Architecture:** Four units with one-way dependencies — `main` wires `ble_uart` (NimBLE, Nordic UART Service) through `textwrap` (pure C, host-testable) into `display` (esp_lcd ST7789, off-screen framebuffer, single blit). A Swift CLI on the Mac writes to the GATT characteristic.

**Tech Stack:** ESP-IDF 5.5.5, NimBLE, esp_lcd, C11; Swift 6.3.3 + CoreBluetooth; Python 3 + PIL for one-off font generation.

**Spec:** `docs/superpowers/specs/2026-09-05-ble-screen-design.md`

## Global Constraints

- Target `esp32s3`. IDF at `~/esp/esp-idf`, env `. ~/esp/esp-idf/export.sh`.
- **Flash the app only**, to `0x10000`. Never write the bootloader, partition table, or `otadata` — TinyUF2 at `0x2d0000` is the recovery path.
- Never pass `--baud 921600` to esptool; USB-Serial-JTAG drops the port.
- Pins: `TFT_I2C_POWER`=21, `TFT_CS`=7, `TFT_DC`=39, `TFT_RST`=40, `TFT_BACKLITE`=45, `SCK`=36, `MOSI`=35.
- Panel: ST7789, 240×135, gap (40, 53), swapped when rotation swaps X/Y.
- Font 8×16 → **30 columns × 8 rows**. ASCII 32..126 only; other bytes render `?`.
- Truncation marker is `...` (three ASCII dots), never `…`.
- NUS UUIDs: service `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`, RX `6E400002-...`. Advertised name `ESP32-Screen`.
- Message cap 512 bytes. Chunk reassembly flushes after **50 ms** idle.

---

### Task 1: `textwrap` (pure C, host-tested)

**Files:**
- Create: `main/textwrap.h`, `main/textwrap.c`
- Test: `host_tests/test_textwrap.c`, `host_tests/Makefile`

**Interfaces:**
- Consumes: nothing.
- Produces: `size_t textwrap(const char *s, size_t cols, size_t max_lines, char out[][TW_MAX_COLS + 1])` returning line count; `TW_MAX_COLS` = 64.

- [ ] **Step 1: Write the failing test** — `host_tests/test_textwrap.c` covering: empty input → 0 lines; short string → 1 line; exact-width; word wrap; over-long word broken mid-word; overflow marked `...`; non-ASCII → `?`; `\n` forced break.
- [ ] **Step 2: Run it and confirm it fails** — `make -C host_tests` fails to link (`textwrap` undefined).
- [ ] **Step 3: Implement `textwrap.c`** — greedy wrap; sanitize bytes outside 32..126 to `?`; on overflow write `...` into the last three cells of the final line.
- [ ] **Step 4: Run and confirm pass** — `make -C host_tests && ./host_tests/test_textwrap` prints `all tests passed`.
- [ ] **Step 5: Commit.**

### Task 2: 8×16 font generation

**Files:**
- Create: `tools/gen_font.py`, `main/font8x16.h` (generated, committed)
- Test: `host_tests/test_font.c`

**Interfaces:**
- Produces: `static const uint8_t font8x16[95][16]` — ASCII 32..126, one byte per row, MSB = leftmost pixel.

- [ ] **Step 1:** Write `tools/gen_font.py` rendering Menlo at size 14 via PIL, thresholding at 110, vertically centred in the 8×16 cell.
- [ ] **Step 2:** Run `/usr/bin/python3 tools/gen_font.py > main/font8x16.h` (PIL 11.1.0 is in `/usr/bin/python3`; the IDF env has none).
- [ ] **Step 3:** Write `host_tests/test_font.c` asserting the table has 95 entries, `' '` is all zero, and `'A'` and `'g'` are non-empty.
- [ ] **Step 4:** Run and confirm pass.
- [ ] **Step 5:** Commit both script and generated header.

### Task 3: ESP-IDF skeleton + `display`

**Files:**
- Create: `CMakeLists.txt`, `sdkconfig.defaults`, `partitions.csv`, `main/CMakeLists.txt`, `main/display.h`, `main/display.c`, `main/main.c`

**Interfaces:**
- Consumes: `textwrap`, `font8x16`.
- Produces: `esp_err_t display_init(void)`, `void display_show_text(const char *utf8)`.

- [ ] **Step 1:** Write the project files; `partitions.csv` mirrors the on-flash layout exactly and is never flashed. `sdkconfig.defaults` sets `CONFIG_IDF_TARGET="esp32s3"`, `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, custom partition table, 4 MB flash.
- [ ] **Step 2:** Implement `display.c` — power/backlight GPIOs high, SPI2 bus, `esp_lcd_new_panel_st7789`, `invert_color(true)`, `swap_xy(true)`, `mirror(false, true)`, `set_gap(53, 40)`, DMA framebuffer, glyph blit.
- [ ] **Step 3:** `main.c` calls `display_init()` then `display_show_text("DISPLAY OK")`.
- [ ] **Step 4:** `idf.py build`, then flash **app only**: `esptool.py --port <port> write_flash 0x10000 build/screen.bin`.
- [ ] **Step 5:** Visually confirm text on the panel; if shifted, try gap (40, 53) and the other mirror combination.
- [ ] **Step 6:** Commit.

### Task 4: `ble_uart` (NimBLE, NUS, reassembly)

**Files:**
- Create: `main/ble_uart.h`, `main/ble_uart.c`
- Modify: `main/CMakeLists.txt`, `sdkconfig.defaults`, `main/main.c`

**Interfaces:**
- Produces: `esp_err_t ble_uart_start(void (*on_message)(const char *text, size_t len))`.

- [ ] **Step 1:** Add `CONFIG_BT_ENABLED=y`, `CONFIG_BT_NIMBLE_ENABLED=y`, `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1` to `sdkconfig.defaults`.
- [ ] **Step 2:** Implement the GATT server with the NUS UUIDs, advertising as `ESP32-Screen`; accumulate writes into a 512-byte buffer, flushing via a 50 ms `esp_timer` one-shot.
- [ ] **Step 3:** Temporarily log each message with `ESP_LOGI` instead of displaying it.
- [ ] **Step 4:** Build, flash app only, and verify from a generic BLE app (LightBlue) that writes appear in the serial log — isolating firmware from Mac-side bugs.
- [ ] **Step 5:** Commit.

### Task 5: Wire BLE → display

**Files:**
- Modify: `main/main.c`

- [ ] **Step 1:** Replace the log callback with `display_show_text`.
- [ ] **Step 2:** Show `READY` on boot so the panel proves liveness before any message arrives.
- [ ] **Step 3:** Build, flash, verify end-to-end from a BLE app.
- [ ] **Step 4:** Commit.

### Task 6: Swift `tell` client

**Files:**
- Create: `mac/tell.swift`, `mac/build.sh`

- [ ] **Step 1:** Implement scan-by-service-UUID → connect → discover → write → exit; 10 s timeout, non-zero exit with stderr message on failure. Read text from `argv[1]` or stdin. Truncate over 512 bytes with a warning.
- [ ] **Step 2:** `swiftc -O mac/tell.swift -o mac/tell`.
- [ ] **Step 3:** Run `mac/tell "hello"` and confirm it appears on the panel.
- [ ] **Step 4:** Commit.

### Task 7: End-to-end verification

- [ ] **Step 1:** Short message.
- [ ] **Step 2:** Message long enough to force wrapping across several lines.
- [ ] **Step 3:** Message over 8 lines — confirm `...` truncation marker.
- [ ] **Step 4:** Message over 512 bytes — confirm client-side warning.
- [ ] **Step 5:** Empty message — confirm the screen clears.
- [ ] **Step 6:** Commit any fixes.

## Self-Review

- **Spec coverage:** architecture (T1–T5), BLE protocol and chunking (T4), display behaviour and wrapping (T1–T3), Mac client (T6), error handling (T1, T4, T6), testing (T1, T2, T7), build/flash and recovery (Global Constraints, T3).
- **Type consistency:** `textwrap` signature identical in T1 and T3; `display_show_text` identical in T3 and T5; `ble_uart_start` callback shape identical in T4 and T5.
- **Placeholders:** none.
