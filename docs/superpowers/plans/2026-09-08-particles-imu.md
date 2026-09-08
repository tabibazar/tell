# Tilt-driven particles on the Feather — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the `small` board — an Adafruit Feather ESP32-S3 TFT with a QMI8658 attached — a particle simulation that falls whichever way the board is tilted, and make it that board's screensaver.

**Architecture:** A QMI8658 I2C driver shaped like the existing `gt911.c`, attached to the shared `i2cbus`; a pure-C `particles` unit that takes a gravity vector and a `canvas_t` and is tested on the host; and wiring in `main.c` that reads the sensor, maps its axes into panel space, low-passes, steps and blits. The I2C pins become Kconfig integers because `i2cbus.c` currently hardcodes CrowPanel wiring.

**Tech Stack:** ESP-IDF 5.5.5, `esp_driver_i2c` (the `i2c_master_*` API), the project's own `canvas`, plain C11 host tests built by `host_tests/Makefile`.

**Spec:** `docs/superpowers/specs/2026-09-08-particles-imu-design.md`

## Global Constraints

- **Flash the app only, at `0x10000`, on the Feather.** `idf.py flash` also writes the bootloader and partition table and destroys the TinyUF2 recovery drive. Use `esptool.py --port /dev/cu.usbmodem* write_flash 0x10000 build/screen.bin`.
- **Never raise the baud rate.** Not `921600`, not `460800`. Use the default.
- **Source `tools/idf-env.sh`, never `export.sh`.** `export.sh` derives its virtualenv name from whatever `python3` currently is.
- **Check `build/screen.bin`'s timestamp before flashing.** A failed build leaves the previous image in place and esptool will flash it happily.
- **Check the image size moves when new units land.** A config that silently fails compiles and runs perfectly; `font.h` once selected the wrong font this way.
- **Any file testing a `CONFIG_*` macro must `#include "sdkconfig.h"`.** Same trap.
- **The last argument to `i2c_master_transmit_receive` and `i2c_master_transmit` is milliseconds, not ticks.** Pass a plain integer such as `50`. `pdMS_TO_TICKS(20)` yields 2, which the driver rounds to a zero-tick wait and reports as `ESP_ERR_INVALID_STATE`.
- **The board is `small`, on `/dev/cu.usbmodem*`, USB serial `68:EE:8F:DA:62:28`.** Its console is USB-Serial-JTAG and works; `idf.py monitor` is available on this board, unlike the CrowPanel.
- Host tests run with `make -C host_tests && for t in host_tests/test_*; do [ -x "$t" ] && "$t"; done`.

---

### Task 1: Find the IMU

Nothing is built on a sensor that has not answered. This task makes the I2C pins configurable and puts a scan on the board, and **stops the plan** if the QMI8658 does not reply.

**Files:**
- Modify: `main/Kconfig.projbuild`
- Modify: `main/i2cbus.c:1-32`
- Modify: `main/main.c` (temporary scan, removed in Task 2)

**Interfaces:**
- Consumes: `i2cbus_init()`, `i2cbus_probe(uint8_t)`, `i2cbus_handle()` — unchanged signatures.
- Produces: `CONFIG_SCREEN_I2C_SDA` and `CONFIG_SCREEN_I2C_SCL`, per-board defaults, used by `i2cbus.c` only.

- [ ] **Step 1: Add the pin options to Kconfig**

Append inside the existing `menu "Screen board"` in `main/Kconfig.projbuild`, after `SCREEN_DEVICE_NAME` and before `endmenu`:

```
config SCREEN_I2C_SDA
    int "I2C SDA pin"
    default 19 if SCREEN_BOARD_CROWPANEL_7
    default 42
    help
        The CrowPanel wires its GT911 touch controller to GPIO19/20. The
        Feather's STEMMA QT port is GPIO42/41, from Adafruit's board variant.

config SCREEN_I2C_SCL
    int "I2C SCL pin"
    default 20 if SCREEN_BOARD_CROWPANEL_7
    default 41
```

- [ ] **Step 2: Use them in `i2cbus.c`**

Replace the two `#define`s at the top of `main/i2cbus.c` and add the include. The include is not optional — see Global Constraints.

```c
#include "i2cbus.h"

#include "esp_log.h"
#include "sdkconfig.h"

/* Board dependent: the CrowPanel wires its GT911 to GPIO19/20, the Feather's
   STEMMA QT port is GPIO42/41. Defaults live in Kconfig.projbuild. */
#define PIN_SDA CONFIG_SCREEN_I2C_SDA
#define PIN_SCL CONFIG_SCREEN_I2C_SCL
```

Delete the old `#define PIN_SCL 20` / `#define PIN_SDA 19` and the comment above them.

- [ ] **Step 3: Add a temporary scan to `app_main`**

In `main/main.c`, immediately after the `display_init()` block in `app_main`, insert:

```c
#ifdef CONFIG_SCREEN_BOARD_FEATHER_S3_TFT
    /* TEMPORARY (Task 1 of the particles plan): find out whether the QMI8658
       is still attached, and at which address. Removed in Task 2. */
    if (i2cbus_init() == ESP_OK) {
        ESP_LOGI(TAG, "i2c scan on SDA %d / SCL %d",
                 CONFIG_SCREEN_I2C_SDA, CONFIG_SCREEN_I2C_SCL);
        for (uint8_t a = 0x08; a < 0x78; a++)
            if (i2cbus_probe(a)) ESP_LOGI(TAG, "  device at 0x%02X", a);
        /* A stuck-low SDA acknowledges every address, so a hit means nothing
           unless an address nothing should answer stays silent. */
        ESP_LOGI(TAG, "  bogus 0x33 answered: %s",
                 i2cbus_probe(0x33) ? "YES - BUS IS STUCK" : "no (good)");
    } else {
        ESP_LOGE(TAG, "i2c bus failed to start");
    }
#endif
```

Add `#include "i2cbus.h"` to the includes at the top of `main/main.c`.

- [ ] **Step 4: Build and check the binary is new**

```bash
. tools/idf-env.sh
idf.py build && ls -l build/screen.bin
```

Expected: `Project build complete`, and `screen.bin`'s timestamp is from this minute.

- [ ] **Step 5: Flash and read the scan**

```bash
esptool.py --port /dev/cu.usbmodem* write_flash 0x10000 build/screen.bin
idf.py -p /dev/cu.usbmodem* monitor
```

Expected: a `device at 0x6A` or `device at 0x6B` line, and `bogus 0x33 answered: no (good)`.

- [ ] **Step 6: Confirm it is the part we think it is**

Add this below the scan, still inside the `#ifdef`, rebuild, reflash, and read the monitor:

```c
    {
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = 0x6B,      /* try 0x6A if the scan said 0x6A */
            .scl_speed_hz = 100000,
        };
        i2c_master_dev_handle_t h;
        if (i2c_master_bus_add_device(i2cbus_handle(), &dev, &h) == ESP_OK) {
            uint8_t reg = 0x00, id[2] = {0, 0};
            esp_err_t e = i2c_master_transmit_receive(h, &reg, 1, id, 2, 50);
            ESP_LOGI(TAG, "whoami read %s: 0x%02X rev 0x%02X",
                     esp_err_to_name(e), id[0], id[1]);
        }
    }
```

Expected: `whoami read ESP_OK: 0x05 rev 0x7B`. `0x05` is the QMI8658's WHO_AM_I; `0x7B` is the revision the stock firmware printed at us on day one.

- [ ] **Step 7: Stop or continue**

If the scan found nothing, or the bogus address answered, or WHO_AM_I is not `0x05`: **stop here and report to Reza.** Say which pins were scanned, what answered, and that the sensor needs locating before anything else is written. Do not proceed to Task 2.

If it answered: record the address in the commit message and continue.

- [ ] **Step 8: Commit**

```bash
git add main/Kconfig.projbuild main/i2cbus.c main/main.c
git commit -m "Make the I2C pins board dependent, and find the QMI8658

The Feather's STEMMA QT bus is GPIO42/41 and i2cbus hardcoded the
CrowPanel's GPIO19/20. Scan output: QMI8658 at 0x6B, WHO_AM_I 0x05,
revision 0x7B. The bogus address stayed silent, so the bus is sound."
```

---

### Task 2: The QMI8658 driver

**Files:**
- Create: `main/qmi8658.h`, `main/qmi8658.c`
- Modify: `main/CMakeLists.txt:9`
- Modify: `main/main.c` (replace the Task 1 scan with a raw readout on the panel)

**Interfaces:**
- Consumes: `i2cbus_init()`, `i2cbus_handle()`, `i2cbus_probe()` from Task 1.
- Produces:
  ```c
  typedef struct { float ax, ay, az; float gx, gy, gz; } qmi8658_sample_t;
  esp_err_t qmi8658_init(void);
  bool      qmi8658_present(void);
  esp_err_t qmi8658_read(qmi8658_sample_t *out);
  const char *qmi8658_debug(void);
  ```
  Accelerometer in g, gyroscope in degrees per second. Task 4 consumes `qmi8658_read` and `qmi8658_present`.

- [ ] **Step 1: Write the header**

`main/qmi8658.h`:

```c
#ifndef QMI8658_H
#define QMI8658_H

#include "esp_err.h"
#include <stdbool.h>

/* QMI8658 six-axis IMU on the Feather's STEMMA QT bus. Accelerometer in g,
   gyroscope in degrees per second, in the sensor's own frame. Mapping those
   axes onto the panel is the caller's job, because it depends on how the
   breakout is oriented relative to the screen. */
typedef struct {
    float ax, ay, az;
    float gx, gy, gz;
} qmi8658_sample_t;

/* Probes both addresses, checks WHO_AM_I, and configures both sensors.
   Returns ESP_ERR_NOT_FOUND when no IMU is attached, which is not fatal:
   the board simply does without. */
esp_err_t qmi8658_init(void);

bool qmi8658_present(void);

esp_err_t qmi8658_read(qmi8658_sample_t *out);

/* One line of state, for drawing on the panel during bring-up. */
const char *qmi8658_debug(void);

#endif /* QMI8658_H */
```

- [ ] **Step 2: Write the driver**

`main/qmi8658.c`:

```c
#include "qmi8658.h"

#include "i2cbus.h"
#include "esp_log.h"

#include <stdio.h>

/* SA0 low gives 0x6A, high gives 0x6B. Which one this breakout uses depends
   on its pull, so probe both, as gt911.c does for the same reason. */
#define ADDR_A 0x6A
#define ADDR_B 0x6B

#define REG_WHOAMI   0x00
#define REG_REVISION 0x01
#define REG_CTRL1    0x02
#define REG_CTRL2    0x03      /* accelerometer range and rate */
#define REG_CTRL3    0x04      /* gyroscope range and rate */
#define REG_CTRL7    0x08      /* enable bits */
#define REG_STATUS0  0x2E
#define REG_AX_L     0x35      /* twelve bytes: ax ay az gx gy gz, LE int16 */

#define WHOAMI_QMI8658 0x05

/* CTRL2 0x13: accelerometer +/-4 g, 235 Hz. CTRL3 0x43: gyroscope +/-512 dps,
   235 Hz. Those ranges set the scale factors below. */
#define CTRL2_VALUE 0x13
#define CTRL3_VALUE 0x43
#define ACCEL_LSB_PER_G   8192.0f    /* 32768 / 4 */
#define GYRO_LSB_PER_DPS    64.0f    /* 32768 / 512 */

static const char *TAG = "qmi8658";
static i2c_master_dev_handle_t s_dev;
static bool s_present;
static uint8_t s_addr;
static uint8_t s_revision;
static int s_reads_ok, s_read_errs;
static char s_dbg[80];

/* The last argument is milliseconds, not ticks. Passing pdMS_TO_TICKS here
   yields 2, which the driver rounds to a zero-tick wait, so every
   transaction returns ESP_ERR_INVALID_STATE without waiting. */
static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 50);
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t out[2] = { reg, value };
    return i2c_master_transmit(s_dev, out, sizeof out, 50);
}

esp_err_t qmi8658_init(void)
{
    esp_err_t err = i2cbus_init();
    if (err != ESP_OK) return err;

    /* If an address nothing should answer also ACKs, SDA is stuck low and
       "found" means nothing. */
    if (i2cbus_probe(0x33)) {
        ESP_LOGE(TAG, "bogus address 0x33 answered; the bus is stuck");
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t addrs[2] = { ADDR_A, ADDR_B };
    for (int i = 0; i < 2; i++) {
        if (!i2cbus_probe(addrs[i])) continue;

        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(i2cbus_handle(), &dev, &s_dev) != ESP_OK)
            continue;

        uint8_t id[2] = {0, 0};
        if (read_regs(REG_WHOAMI, id, sizeof id) != ESP_OK || id[0] != WHOAMI_QMI8658) {
            ESP_LOGW(TAG, "0x%02X answered but WHO_AM_I is 0x%02X", addrs[i], id[0]);
            i2c_master_bus_rm_device(s_dev);
            s_dev = NULL;
            continue;
        }

        s_addr = addrs[i];
        s_revision = id[1];

        /* Address auto-increment, so the twelve data bytes come out in one
           transaction rather than twelve. */
        if (write_reg(REG_CTRL1, 0x40) != ESP_OK) return ESP_FAIL;
        if (write_reg(REG_CTRL2, CTRL2_VALUE) != ESP_OK) return ESP_FAIL;
        if (write_reg(REG_CTRL3, CTRL3_VALUE) != ESP_OK) return ESP_FAIL;
        if (write_reg(REG_CTRL7, 0x03) != ESP_OK) return ESP_FAIL;  /* aEN | gEN */

        s_present = true;
        ESP_LOGI(TAG, "QMI8658 at 0x%02X, revision 0x%02X", s_addr, s_revision);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "no QMI8658 at 0x%02X or 0x%02X; continuing without it",
             ADDR_A, ADDR_B);
    return ESP_ERR_NOT_FOUND;
}

bool qmi8658_present(void) { return s_present; }

esp_err_t qmi8658_read(qmi8658_sample_t *out)
{
    if (!s_present) return ESP_ERR_INVALID_STATE;

    uint8_t b[12];
    esp_err_t err = read_regs(REG_AX_L, b, sizeof b);
    if (err != ESP_OK) { s_read_errs++; return err; }
    s_reads_ok++;

    int16_t raw[6];
    for (int i = 0; i < 6; i++)
        raw[i] = (int16_t)((uint16_t)b[2 * i] | ((uint16_t)b[2 * i + 1] << 8));

    out->ax = raw[0] / ACCEL_LSB_PER_G;
    out->ay = raw[1] / ACCEL_LSB_PER_G;
    out->az = raw[2] / ACCEL_LSB_PER_G;
    out->gx = raw[3] / GYRO_LSB_PER_DPS;
    out->gy = raw[4] / GYRO_LSB_PER_DPS;
    out->gz = raw[5] / GYRO_LSB_PER_DPS;
    return ESP_OK;
}

const char *qmi8658_debug(void)
{
    if (!s_present) {
        snprintf(s_dbg, sizeof s_dbg, "no imu");
        return s_dbg;
    }
    qmi8658_sample_t s;
    if (qmi8658_read(&s) != ESP_OK) {
        snprintf(s_dbg, sizeof s_dbg, "0x%02X errs=%d", s_addr, s_read_errs);
        return s_dbg;
    }
    snprintf(s_dbg, sizeof s_dbg, "%+.2f %+.2f %+.2f", s.ax, s.ay, s.az);
    return s_dbg;
}
```

- [ ] **Step 3: Add it to the build**

In `main/CMakeLists.txt`, add `"qmi8658.c"` to the `SRCS` list, after `"i2cbus.c"`.

- [ ] **Step 4: Replace the Task 1 scan with a raw readout**

Delete the temporary scan block from `app_main` and put this in its place, so the panel shows the accelerometer while you tilt the board. It is removed in Task 4.

```c
#ifdef CONFIG_SCREEN_BOARD_FEATHER_S3_TFT
    /* TEMPORARY (Task 2 of the particles plan): show the raw accelerometer so
       the axis mapping can be read off the board. Removed in Task 4. */
    qmi8658_init();
    for (;;) {
        canvas_clear(display_canvas());
        canvas_puts(display_canvas(), 0, 1, qmi8658_debug(), PAL_FG);
        canvas_puts(display_canvas(), 0, 3, "tilt me", PAL_DIM);
        display_blit();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
#endif
```

Add `#include "qmi8658.h"` at the top of `main/main.c`.

- [ ] **Step 5: Build, check the image grew, flash**

```bash
. tools/idf-env.sh
idf.py build && ls -l build/screen.bin
esptool.py --port /dev/cu.usbmodem* write_flash 0x10000 build/screen.bin
```

Expected: `screen.bin` is both newly timestamped and larger than the Task 1 image — the driver is roughly 2 KB of new code. If it did not grow, the file is not in `SRCS`.

- [ ] **Step 6: Read the axis mapping off the board**

Hold the board upright, facing you, and note the three numbers. Then, one at a time:

- Tilt the **top of the screen away** from you.
- Tilt the board **left**, so the left edge drops.
- Lay it **flat, screen up**.

Write down, in a comment you will paste into Task 4, which sensor axis reads about `-1.00` when the board is upright — that is the one pointing down the screen — and which reads about `+1.00` when the left edge drops.

At rest one axis should read close to `1.00` or `-1.00` and the other two close to zero. If all three wander, or the numbers do not move when you tilt, stop and report: the part answered but is not sampling.

- [ ] **Step 7: Commit**

```bash
git add main/qmi8658.c main/qmi8658.h main/CMakeLists.txt main/main.c
git commit -m "Add a QMI8658 driver for the Feather's IMU

Shaped like gt911.c: attaches to the shared i2cbus, probes both
addresses and one that nothing should answer, checks WHO_AM_I before
believing anything, and reports absence rather than failing.

Accelerometer +/-4 g and gyroscope +/-512 dps, both at 235 Hz, read in
one auto-incremented transaction."
```

---

### Task 3: The particle simulation

Pure C, no ESP-IDF headers, so it is written and debugged entirely on the host before the board sees it.

**Files:**
- Create: `main/particles.h`, `main/particles.c`
- Create: `host_tests/test_particles.c`
- Modify: `host_tests/Makefile`
- Modify: `main/CMakeLists.txt`

**Interfaces:**
- Consumes: `canvas_t` and `canvas_fill_rect` from `main/canvas.h`.
- Produces:
  ```c
  #define PARTICLES_MAX 240
  typedef struct { float x, y, vx, vy; uint16_t colour; } particle_t;
  typedef struct { particle_t p[PARTICLES_MAX]; int n, w, h; uint32_t rng; } particles_t;
  void particles_init(particles_t *s, int n, int w, int h, uint32_t seed);
  void particles_step(particles_t *s, float gx, float gy, float dt);
  void particles_draw(const particles_t *s, canvas_t *c);
  ```
  Task 4 consumes all three.

- [ ] **Step 1: Write the header**

`main/particles.h`:

```c
#ifndef PARTICLES_H
#define PARTICLES_H

#include "canvas.h"

#include <stdint.h>

/*
 * A few hundred particles falling under a gravity vector supplied by the
 * caller. Knows nothing about accelerometers or panels, so it builds and
 * runs on the host like canvas and usagedata do.
 *
 * Particles do not collide with each other. A few hundred of them at twenty
 * frames a second has no budget for it, and a heap that slumps under gravity
 * reads correctly without it.
 */

#define PARTICLES_MAX 240

typedef struct {
    float x, y;        /* panel pixels */
    float vx, vy;      /* pixels per second */
    uint16_t colour;
} particle_t;

typedef struct {
    particle_t p[PARTICLES_MAX];
    int n;
    int w, h;
    uint32_t rng;
} particles_t;

/* Scatters `n` particles over a w x h panel. `n` is clamped to PARTICLES_MAX. */
void particles_init(particles_t *s, int n, int w, int h, uint32_t seed);

/* Advances by `dt` seconds under gravity (gx, gy) in pixels per second
   squared, in panel coordinates: +x right, +y down. */
void particles_step(particles_t *s, float gx, float gy, float dt);

/* Draws each particle as a 2x2 block. Clears the canvas first. */
void particles_draw(const particles_t *s, canvas_t *c);

#endif /* PARTICLES_H */
```

- [ ] **Step 2: Write the failing test**

`host_tests/test_particles.c`:

```c
#include "particles.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

#define W 240
#define H 135

static float centre_x(const particles_t *s)
{
    float t = 0;
    for (int i = 0; i < s->n; i++) t += s->p[i].x;
    return t / (float)s->n;
}

static float centre_y(const particles_t *s)
{
    float t = 0;
    for (int i = 0; i < s->n; i++) t += s->p[i].y;
    return t / (float)s->n;
}

static float energy(const particles_t *s)
{
    float t = 0;
    for (int i = 0; i < s->n; i++)
        t += s->p[i].vx * s->p[i].vx + s->p[i].vy * s->p[i].vy;
    return t;
}

static void run(particles_t *s, float gx, float gy, int steps)
{
    for (int i = 0; i < steps; i++) particles_step(s, gx, gy, 1.0f / 30.0f);
}

int main(void)
{
    particles_t s;

    /* Bounds, under gravity pointing each of four ways. */
    const float g[4][2] = { {0, 400}, {0, -400}, {400, 0}, {-400, 0} };
    for (int d = 0; d < 4; d++) {
        particles_init(&s, 200, W, H, 12345u + (unsigned)d);
        run(&s, g[d][0], g[d][1], 1000);
        int inside = 1;
        for (int i = 0; i < s.n; i++)
            if (s.p[i].x < 0 || s.p[i].x > W || s.p[i].y < 0 || s.p[i].y > H)
                inside = 0;
        expect("particles stay on the panel", inside);
    }

    /* Damping is real: with no gravity, motion dies away. */
    particles_init(&s, 200, W, H, 999u);
    for (int i = 0; i < s.n; i++) { s.p[i].vx = 50.0f; s.p[i].vy = -30.0f; }
    float before = energy(&s);
    run(&s, 0, 0, 300);
    expect("kinetic energy decays without gravity", energy(&s) < before * 0.5f);

    /* Gravity to the right piles them up on the right. */
    particles_init(&s, 200, W, H, 42u);
    float x0 = centre_x(&s);
    run(&s, 600, 0, 200);
    expect("gravity right moves the centre of mass right", centre_x(&s) > x0 + 10.0f);

    /* And down is down, which is the sign error that would otherwise ship. */
    particles_init(&s, 200, W, H, 42u);
    float y0 = centre_y(&s);
    run(&s, 0, 600, 200);
    expect("gravity down moves the centre of mass down", centre_y(&s) > y0 + 10.0f);

    /* Drawing writes only inside the framebuffer. */
    static uint16_t guarded[8 + W * H + 8];
    memset(guarded, 0xAB, sizeof guarded);
    canvas_t c;
    canvas_init(&c, guarded + 8, W, H, 1);
    particles_init(&s, 200, W, H, 7u);
    run(&s, 0, 400, 100);
    particles_draw(&s, &c);
    int guards_intact = 1;
    for (int i = 0; i < 8; i++)
        if (guarded[i] != 0xABAB || guarded[8 + W * H + i] != 0xABAB)
            guards_intact = 0;
    expect("draw stays inside the framebuffer", guards_intact);

    /* n is clamped rather than overrunning the array. */
    particles_init(&s, PARTICLES_MAX + 500, W, H, 1u);
    expect("n is clamped to PARTICLES_MAX", s.n == PARTICLES_MAX);

    printf("%s\n", failures ? "FAILURES" : "all tests passed");
    return failures ? 1 : 0;
}
```

Add the rule to `host_tests/Makefile`: append `test_particles` to the `all:` line and to the `clean:` line, and add

```make
test_particles: test_particles.c ../main/particles.c ../main/canvas.c ../main/textwrap.c
	cc $(CFLAGS) -o $@ $^
```

- [ ] **Step 3: Run it and watch it fail**

```bash
make -C host_tests test_particles
```

Expected: FAIL — `fatal error: 'particles.h' file not found` is fine at this point if the header is not written yet; once it is, the failure becomes `Undefined symbols: _particles_init`.

- [ ] **Step 4: Write the implementation**

`main/particles.c`:

```c
#include "particles.h"

#include "palette.h"

/* Bounce loses this much speed, so the pile settles instead of ringing. */
#define RESTITUTION 0.45f
/* Drag per second, applied as a multiplier, so motion dies without gravity. */
#define DRAG 0.90f
/* Below this speed a particle against a wall is simply stopped, which is what
   stops a heap shivering for ever on a noisy accelerometer. */
#define SLEEP_SPEED 6.0f

#define PARTICLE_SIZE 2

static uint32_t next_rand(particles_t *s)
{
    s->rng = s->rng * 1103515245u + 12345u;
    return s->rng >> 16;
}

static float frand(particles_t *s, float lo, float hi)
{
    return lo + (float)(next_rand(s) % 10000u) / 10000.0f * (hi - lo);
}

void particles_init(particles_t *s, int n, int w, int h, uint32_t seed)
{
    if (n > PARTICLES_MAX) n = PARTICLES_MAX;
    if (n < 0) n = 0;
    s->n = n;
    s->w = w;
    s->h = h;
    s->rng = seed ? seed : 1u;

    /* Six colours from the project's Okabe-Ito set, so the animation belongs
       to the same palette as the charts. */
    static const uint16_t colours[6] = {
        PAL_A0, PAL_A1, PAL_A2, PAL_A3, PAL_A4, PAL_A5
    };

    for (int i = 0; i < n; i++) {
        s->p[i].x = frand(s, 1.0f, (float)w - 1.0f);
        s->p[i].y = frand(s, 1.0f, (float)h - 1.0f);
        s->p[i].vx = frand(s, -20.0f, 20.0f);
        s->p[i].vy = frand(s, -20.0f, 20.0f);
        s->p[i].colour = colours[next_rand(s) % 6u];
    }
}

static void bounce(float *pos, float *vel, float limit)
{
    if (*pos < 0.0f) {
        *pos = 0.0f;
        *vel = -*vel * RESTITUTION;
    } else if (*pos > limit) {
        *pos = limit;
        *vel = -*vel * RESTITUTION;
    } else {
        return;
    }
    if (*vel < SLEEP_SPEED && *vel > -SLEEP_SPEED) *vel = 0.0f;
}

void particles_step(particles_t *s, float gx, float gy, float dt)
{
    /* Drag as a per-second multiplier, applied linearly over dt. At the frame
       rates this runs at, dt is small enough that the approximation is
       indistinguishable from the exponential and much cheaper. */
    float damp = 1.0f - (1.0f - DRAG) * dt;
    if (damp < 0.0f) damp = 0.0f;

    float maxx = (float)s->w;
    float maxy = (float)s->h;

    for (int i = 0; i < s->n; i++) {
        particle_t *p = &s->p[i];
        p->vx = (p->vx + gx * dt) * damp;
        p->vy = (p->vy + gy * dt) * damp;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        bounce(&p->x, &p->vx, maxx);
        bounce(&p->y, &p->vy, maxy);
    }
}

void particles_draw(const particles_t *s, canvas_t *c)
{
    canvas_clear(c);
    for (int i = 0; i < s->n; i++)
        canvas_fill_rect(c, (int)s->p[i].x, (int)s->p[i].y,
                         PARTICLE_SIZE, PARTICLE_SIZE, s->p[i].colour);
}
```

- [ ] **Step 5: Run the tests until they pass**

```bash
make -C host_tests test_particles && ./host_tests/test_particles
```

Expected: every line `ok`, then `all tests passed`.

If "kinetic energy decays" fails, `DRAG` is too close to 1. If "stay on the panel" fails, `bounce` is not clamping before the next step. Fix the cause, not the test.

- [ ] **Step 6: Run the whole host suite, so nothing else broke**

```bash
make -C host_tests && for t in host_tests/test_*; do [ -x "$t" ] && "$t"; done
```

Expected: `all tests passed` from each of the seven binaries.

- [ ] **Step 7: Add it to the firmware build**

In `main/CMakeLists.txt`, add `"particles.c"` to `SRCS`.

- [ ] **Step 8: Commit**

```bash
git add main/particles.c main/particles.h main/CMakeLists.txt \
        host_tests/test_particles.c host_tests/Makefile
git commit -m "Add a host-tested particle simulation

Pure C, no IDF headers, so it is debugged on the host like canvas and
usagedata. Takes a gravity vector in panel coordinates and knows nothing
about accelerometers, which is what makes it testable with a constant.

No particle-to-particle collision: no budget for it at 240 particles, and
a slumping heap reads correctly without it."
```

---

### Task 4: Wire it into the board

**Files:**
- Modify: `main/pages.h:5-14`
- Modify: `main/usagedata.h:70`, `main/usagedata.c:71-75`
- Modify: `main/main.c`
- Modify: `host_tests/test_pages.c`, `host_tests/test_usagedata.c`

**Interfaces:**
- Consumes: `qmi8658_read`, `qmi8658_present` (Task 2); `particles_init/step/draw` (Task 3).
- Produces: `PAGE_PARTICLES` in `page_t`, and `UD_PARTICLES` in `ud_kind_t`.

- [ ] **Step 1: Write the failing tests for the new page and marker**

In `host_tests/test_pages.c`, add after the existing `FEATHER` block:

```c
    /* The Feather with an IMU: clock, message, particles. */
    pages_init(&p, FEATHER | PAGE_BIT(PAGE_PARTICLES));
    expect("feather+imu starts on the clock", p.current == PAGE_CLOCK);
    expect("advance reaches message", pages_advance(&p, 1000) == PAGE_MESSAGE);
    expect("then particles", pages_advance(&p, 2000) == PAGE_PARTICLES);
    expect("then wraps", pages_advance(&p, 3000) == PAGE_CLOCK);

    /* A board without one never lands there. */
    pages_init(&p, FEATHER);
    pages_show(&p, PAGE_PARTICLES, 1000);
    expect("particles ignored when unavailable", p.current == PAGE_CLOCK);
```

In `host_tests/test_usagedata.c`, add:

```c
    expect("particles marker recognised",
           usagedata_parse(&d, "!particles\n", 1000) == UD_PARTICLES);
```

- [ ] **Step 2: Run them and watch them fail**

```bash
make -C host_tests test_pages test_usagedata
```

Expected: FAIL — `'PAGE_PARTICLES' undeclared` and `'UD_PARTICLES' undeclared`.

- [ ] **Step 3: Add the page**

In `main/pages.h`, add to the enum, before `PAGE_COUNT`:

```c
    PAGE_TODAY,
    PAGE_PARTICLES,
    PAGE_COUNT
```

It goes last so the existing page order, which the other tests assert, does not move.

- [ ] **Step 4: Add the marker**

In `main/usagedata.h`, extend the enum:

```c
typedef enum { UD_NONE = 0, UD_STATS, UD_DAILY, UD_CLOCK, UD_TODAY,
               UD_PARTICLES } ud_kind_t;
```

In `main/usagedata.c`, in `usagedata_parse`, add a branch beside the others:

```c
    else if (starts_with(payload, "!today")) kind = UD_TODAY;
    else if (starts_with(payload, "!particles")) return UD_PARTICLES;
    else return UD_NONE;
```

It returns immediately because, unlike the others, it carries no payload to store.

- [ ] **Step 5: Run the tests until they pass**

```bash
make -C host_tests && for t in host_tests/test_*; do [ -x "$t" ] && "$t"; done
```

Expected: every binary reports `all tests passed`.

- [ ] **Step 6: Remove the Task 2 readout and wire the simulation in**

In `main/main.c`, delete the temporary `#ifdef CONFIG_SCREEN_BOARD_FEATHER_S3_TFT` readout loop from Task 2 entirely.

Add near the other file-scope state, below the screensaver block:

```c
/* Particles, on the Feather only, driven by its QMI8658. */
#ifdef CONFIG_SCREEN_BOARD_FEATHER_S3_TFT
static particles_t s_particles;
static bool s_imu;
static int64_t s_particles_last_us;
/* Low-passed gravity in panel coordinates, +x right, +y down. */
static float s_gx, s_gy;

/* PASTE THE AXIS MAPPING OBSERVED IN TASK 2, STEP 6 HERE, as the comment and
   the two expressions below. The placeholder assumes the sensor's -Y points
   down the screen and its +X points right; correct it to what you measured. */
static void gravity_from(const qmi8658_sample_t *s, float *gx, float *gy)
{
    *gx =  s->ax;
    *gy = -s->ay;
}

/* Gravity in pixels per second squared. A full 1 g tilt crosses the short
   axis of the panel in about half a second, which looks like sand rather
   than like a screensaver. */
#define GRAVITY_PX 900.0f
/* First-order low pass, about a 150 ms time constant at 20 fps. Raw
   accelerometer output at rest is noisy enough to make a settled heap
   shiver. */
#define GRAVITY_ALPHA 0.25f

static void draw_particles(canvas_t *c, int64_t now)
{
    float dt = s_particles_last_us
             ? (float)(now - s_particles_last_us) / 1000000.0f : 0.05f;
    s_particles_last_us = now;
    if (dt > 0.2f) dt = 0.2f;      /* a long stall must not teleport anything */

    qmi8658_sample_t sample;
    if (qmi8658_read(&sample) == ESP_OK) {
        float gx, gy;
        gravity_from(&sample, &gx, &gy);
        s_gx += (gx * GRAVITY_PX - s_gx) * GRAVITY_ALPHA;
        s_gy += (gy * GRAVITY_PX - s_gy) * GRAVITY_ALPHA;
    }

    particles_step(&s_particles, s_gx, s_gy, dt);
    particles_draw(&s_particles, c);
    display_blit();
}
#endif
```

Add the includes `#include "particles.h"` and `#include "qmi8658.h"` at the top of `main/main.c`.

- [ ] **Step 7: Offer the page, and make it the screensaver**

In `app_main`, after the existing `available` block, add:

```c
#ifdef CONFIG_SCREEN_BOARD_FEATHER_S3_TFT
    s_imu = qmi8658_init() == ESP_OK;
    if (s_imu) {
        available |= PAGE_BIT(PAGE_PARTICLES);
        particles_init(&s_particles, 200, c->w, c->h, 0xC0FFEEu);
    }
#endif
```

This must come after `canvas_t *c = display_canvas();`, so move that line above the `pages_init` call.

In the main loop, the screensaver branch currently reads:

```c
        bool saver_now = s_synced && pages_saver_active(&s_pages, now);
```

Change it to:

```c
        /* The clock screensaver needs the time; the particles do not, so on a
           board with an IMU the saver runs whether or not a Mac has ever
           connected. */
        bool saver_now = pages_saver_active(&s_pages, now)
                       && (s_synced || SAVER_NEEDS_NO_CLOCK);
```

with, near `TICK_MS`:

```c
#ifdef CONFIG_SCREEN_BOARD_FEATHER_S3_TFT
#define SAVER_NEEDS_NO_CLOCK s_imu
#else
#define SAVER_NEEDS_NO_CLOCK false
#endif
```

Then, inside `if (s_saver) { ... }`, put the particles ahead of the clock:

```c
        if (s_saver) {
#ifdef CONFIG_SCREEN_BOARD_FEATHER_S3_TFT
            if (s_imu) { draw_particles(c, now); continue; }
#endif
            uint32_t secs = timecalc_advance(s_base_secs,
                                             (uint64_t)(now - s_base_us));
            ...unchanged...
        }
```

- [ ] **Step 8: Draw the page when it is the current one**

In the main loop, below the existing `if (s_pages.current == PAGE_CLOCK) draw_clock(c, now);`, add:

```c
#ifdef CONFIG_SCREEN_BOARD_FEATHER_S3_TFT
        if (s_pages.current == PAGE_PARTICLES && s_imu) draw_particles(c, now);
#endif
```

And in `on_message`, beside the other markers:

```c
    if (kind == UD_PARTICLES) {
        /* Unlike the data markers this is a request, so it does take the
           view: someone asked for it. */
        pages_show(&s_pages, PAGE_PARTICLES, now);
        s_drawn_page = PAGE_COUNT;
        return;
    }
```

- [ ] **Step 9: Build, check the image grew, flash**

```bash
. tools/idf-env.sh
idf.py build && ls -l build/screen.bin
esptool.py --port /dev/cu.usbmodem* write_flash 0x10000 build/screen.bin
```

Expected: `screen.bin` newly timestamped and larger than Task 2's.

- [ ] **Step 10: Verify on the board**

```bash
tell --device small "!particles"
```

Expected: the panel fills with coloured specks. **Tilt the board left and they must go left.** If they go the wrong way, or fall sideways when the board is upright, the mapping in `gravity_from` is wrong — fix the two expressions there, not the physics.

Then leave the board alone for five minutes and confirm the particles come back on their own as the screensaver.

- [ ] **Step 11: Confirm nothing else regressed**

```bash
tell --device small "hello"        # message page still appears
idf.py -p /dev/cu.usbmodem* monitor  # the 30s heartbeat still logs
```

Expected: the message displays, and `alive, page N` keeps appearing every thirty seconds. A heartbeat that stops means the animation is starving the loop.

- [ ] **Step 12: Commit**

```bash
git add main/main.c main/pages.h main/usagedata.c main/usagedata.h \
        host_tests/test_pages.c host_tests/test_usagedata.c
git commit -m "Show the particles: as a page, and as the Feather's screensaver

Particles in motion protect the panel better than a clock that hops once
a minute, so on a board with an IMU they replace the drifting clock. That
saver no longer waits for a Mac to set the time, because unlike the clock
the particles do not need it.

A !particles marker summons the page on demand, which also means the
firmware can be told apart from its client by typing one line."
```

---

### Task 5: Tune it, and write down what the board taught us

**Files:**
- Modify: `main/particles.c` (constants only)
- Modify: `main/main.c` (particle count only)
- Modify: `docs/developing.md`, `docs/hardware/feather-esp32s3-tft.md`, `README.md`

- [ ] **Step 1: Measure one frame**

Add temporarily to `draw_particles`, rebuild, flash, and read the monitor:

```c
    int64_t t0 = esp_timer_get_time();
    display_blit();
    static int64_t acc; static int cnt;
    acc += esp_timer_get_time() - t0;
    if (++cnt == 100) { ESP_LOGI(TAG, "blit %lld us", acc / 100); acc = 0; cnt = 0; }
```

Expected: roughly 13,000 us at the 40 MHz SPI clock in `display_st7789.c`.

- [ ] **Step 2: Choose the count and cadence from that number**

If a blit plus a step comfortably fits inside 50 ms, raise the particle count in the `particles_init` call towards `PARTICLES_MAX`. If there is room for 30 fps, shorten the loop delay while particles are running:

```c
        vTaskDelay(pdMS_TO_TICKS(s_saver && s_imu ? 33 : TICK_MS));
```

Remove the timing instrumentation once the numbers are recorded. Judge the result by looking at it: it should read as sand, not as confetti.

- [ ] **Step 3: Try the gyroscope swirl, and keep it only if it earns its place**

The spec allows one garnish: rotation about the axis normal to the screen adds
a tangential nudge, so spinning the board stirs the pile. In `draw_particles`,
after the gravity low pass:

```c
    /* gz is degrees per second about the axis out of the screen. Correct the
       field if Task 2 showed a different axis facing the viewer. */
    float swirl = sample.gz * 0.02f;
    for (int i = 0; i < s_particles.n; i++) {
        float rx = s_particles.p[i].x - (float)c->w / 2.0f;
        float ry = s_particles.p[i].y - (float)c->h / 2.0f;
        s_particles.p[i].vx += -ry * swirl;
        s_particles.p[i].vy +=  rx * swirl;
    }
```

Flash it and spin the board on the desk. If it does not obviously improve the
thing, **delete it** — nothing else depends on it, and a feature that has to be
explained to be noticed is not worth the lines.

- [ ] **Step 4: Document the hardware**

Add to the "Facts the probe cannot see" table in `docs/hardware/feather-esp32s3-tft.md`:

```
| IMU | QMI8658, STEMMA QT, SDA 42 / SCL 41, address 0x6B |
| IMU power | GPIO21 (TFT_I2C_POWER), already driven high for the panel |
| Axis mapping | (record what Task 2 Step 6 measured) |
```

Correct the address and mapping to what was actually observed.

- [ ] **Step 5: Document the trap, if there was one**

`docs/developing.md` has a "Traps, each of which cost an hour or more" section. If anything in this work cost real time — a wrong register, a sign error, an axis that was not what the datasheet implied — add it there in the same voice as the entries around it. If nothing did, add nothing: that section earns its authority by containing only things that actually happened.

Also update its "Next: the DS3231" section, which says the I2C pins are SDA 19 / SCL 20 — that is now board dependent and set in Kconfig.

- [ ] **Step 6: Mention it in the README**

One short paragraph under whatever section describes the boards: the small board has an IMU, tilting it pours the particles, and they take over as its screensaver.

- [ ] **Step 7: Full test run and commit**

```bash
make -C host_tests && for t in host_tests/test_*; do [ -x "$t" ] && "$t"; done
git add -A
git commit -m "Tune the particles and record what the IMU work taught us

Blit measured at N us, so the loop runs at M fps with K particles."
```

Fill in the real numbers.
