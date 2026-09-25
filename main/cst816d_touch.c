/*
 * watch's touch: the Hynitron CST816D capacitive controller at 0x15 on the
 * main I2C bus, which it shares with the QMI8658 and the PCF85063.
 *
 * Modelled on axs5106l_touch.c, and it takes that driver's lesson up front:
 * the AXS5106L ACKed its address from power-up but NACKed every data read
 * until RST was pulsed at init and the register write and the read went as
 * two transactions with a STOP between. Both are done here from the start.
 * The Arduino drivers below read the CST816 that way too (Waveshare's
 * IIC_ReadC8D8 and fbiego's i2c_read both STOP before requesting); only
 * Espressif's uses a repeated start.
 *
 * The register map, read as six bytes from 0x01:
 *
 *   0x01 gesture id      (the chip's own swipe/click guess, ignored here)
 *   0x02 finger count    (0 or 1: the part is single-touch)
 *   0x03 X high, in the LOW nibble     0x04 X low
 *   0x05 Y high, in the LOW nibble     0x06 Y low
 *
 * The high nibbles carry event flags, so only the low four bits are taken.
 * An unmasked flag puts the touch thousands of pixels away.
 *
 * Auto-sleep has to go. Left to itself the chip dozes a few seconds after the
 * last touch and stops answering I2C until a finger wakes it. Polled like
 * this, that would make every idle read a NACK -- and ESP-IDF 5.5 logs each
 * failed transaction as an error -- and the first moments of a touch would
 * be lost to the wake. Register 0xFE (DisAutoSleep) non-zero keeps it awake.
 * SensorLib writes 0x01 there straight after a reset for the CST816S, T, D
 * and CST820 alike; fbiego writes any non-zero value. Waveshare's own driver
 * never touches 0xFE, because their demo reads only on an INT pulse, and
 * this driver polls.
 *
 * Sources, all read before writing this:
 *   - SensorLib TouchDrvCST816.cpp/.h (lewisxhe), as vendored in Waveshare's
 *     ESP32-S3-Touch-LCD-1.69 repo under examples/arduino/libraries: the
 *     0xFE write, chip ids (0xB6 is the CST816D), and the 0xFF quirk below.
 *   - Waveshare ESP32-S3-Touch-LCD-1.69, Arduino_DriveBus Arduino_CST816x:
 *     the 0x01..0x06 register names, and the reset (low 10 ms, then 200 ms).
 *     Their 02_Drawing_board clamps the raw x/y straight onto 240x280 at
 *     rotation 0, which is why there is no swap or scale here.
 *   - fbiego/CST816S (Arduino): the six-byte read from 0x01.
 *   - Espressif esp_lcd_touch_cst816s (esp-bsp): the same layout read from
 *     0x02, and chip id at 0xA7.
 *   - Waveshare's HARDWARE_REFERENCE.md and ESP-IDF BSP for this board: the
 *     RST and INT pins below. The reference names the part a CST816T (id
 *     0xB5), not a D; the id is logged, not checked, so either works, and
 *     the boot log will say which this unit has.
 *
 * RST is GPIO13 and INT GPIO14, as WATCH_TP_RST and WATCH_TP_INT: a -D can
 * override either, and -1 means not driven. Three of Waveshare's sources
 * agree on the pair -- the Arduino pin_config.h (TP_RST 13, TP_INT 14), the
 * ESP-IDF BSP (BSP_LCD_TOUCH_RST/INT, GPIO_NUM_13/14) and the board's
 * HARDWARE_REFERENCE.md -- and neither is among the pins the two hardware
 * revisions move (see the watch spec). Like the panel pins in
 * display_st7789.c, they are still to be confirmed on the unit.
 *
 * RST is not optional in practice. Nothing before this driver has written
 * 0xFE (Waveshare's demo never does, above), so the chip arrives with
 * auto-sleep on, and a flash or a soft restart does not cycle its supply. By
 * the time touch_init runs it has usually dozed off, and a dozing CST816 does
 * not ACK the probe: without the pulse, touch would be gone for that whole
 * boot. The retry in read_point cannot cover it, as it only runs once the
 * probe has passed. Polling only: INT is configured as an input so nothing
 * else drives it, and never read.
 *
 * The panel is native portrait and so is the canvas, so a touch at (x, y)
 * sits under canvas (x, y). If a swipe ever goes the wrong way, or a tap
 * lands mirrored, flip the sign where cx/cy are formed -- it cannot be
 * calibrated without the glass in hand.
 */
#ifdef ESP_PLATFORM

#include "touch.h"

#include "i2cbus.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <stdlib.h>

#ifndef WATCH_TP_RST
#define WATCH_TP_RST 13     /* Waveshare BSP: TP_RST 13, TP_INT 14 */
#endif
#ifndef WATCH_TP_INT
#define WATCH_TP_INT 14
#endif

#define ADDR             0x15
#define REG_DATA         0x01
#define REG_CHIP_ID      0xA7
#define REG_DIS_AUTOSLEEP 0xFE
#define CANVAS_W         240
#define CANVAS_H         280

/* A drag at least this far across is a swipe; a press that moves less is a
   tap. In canvas pixels. */
#define SWIPE_MIN_PX 60
#define TAP_MAX_PX   25

static const char *TAG = "touch";
static i2c_master_dev_handle_t s_dev;
static bool s_present;          /* the address ACKed and the device is attached */
static bool s_answered;         /* ...and a register read has come back */
static bool s_awake;            /* auto-sleep is off */
static int s_wake_tries;        /* retries of that write after init */
static uint8_t s_chip_id;
static int s_read_errs;

/* Gesture state, in canvas coordinates. */
static bool s_down;
static int  s_down_x, s_down_y, s_last_x, s_last_y;
static int  s_raw_x, s_raw_y;
static int  s_pending_swipe;    /* -1 left, +1 right, 0 none */
static bool s_pending_tap;
static int  s_tap_x, s_tap_y;
static int64_t s_start_us;      /* boot grace against a power-up phantom touch */

/* The pin arrives as a parameter rather than being shifted as the macro: a
   constant 1ULL << -1 is a warning, so an error under -Werror, even in a
   branch that never runs. A runtime test also still works if the pin is ever
   given as a GPIO_NUM_ enum, which a preprocessor #if would read as 0. */
static void pulse_reset(int pin)
{
    if (pin < 0) return;
    gpio_config_t rst = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rst);
    gpio_set_level(pin, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(pin, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
}

/*
 * The chip pulls INT low when a finger lands (and, asleep, that is the only
 * sign of life it gives: it NACKs its address until then). An edge flag lets
 * a sleeping controller be read only when it has something to say, instead
 * of NACKing -- and the I2C driver logging an error -- twenty times a second.
 */
static volatile bool s_int_seen;

static void IRAM_ATTR int_edge(void *arg)
{
    (void)arg;
    s_int_seen = true;
}

static void claim_int(int pin)
{
    if (pin < 0) return;
    /* No pull-up on the board (FPC pin 16 runs straight to the GPIO). */
    gpio_config_t irq = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT,
                          .pull_up_en = GPIO_PULLUP_ENABLE,
                          .intr_type = GPIO_INTR_NEGEDGE };
    gpio_config(&irq);
    esp_err_t e = gpio_install_isr_service(0);
    if (e == ESP_OK || e == ESP_ERR_INVALID_STATE)
        gpio_isr_handler_add(pin, int_edge, NULL);
}

/* A register read as two transactions with a STOP between, as the AXS5106L
   needed and as Waveshare's own CST816 driver does it. */
static bool read_regs(uint8_t reg, uint8_t *out, size_t n)
{
    esp_err_t err = i2c_master_transmit(s_dev, &reg, 1, 50);
    if (err == ESP_OK) err = i2c_master_receive(s_dev, out, n, 50);
    if (err != ESP_OK) { s_read_errs++; return false; }
    s_answered = true;
    return true;
}

static bool disable_auto_sleep(void)
{
    const uint8_t w[2] = { REG_DIS_AUTOSLEEP, 0x01 };
    return i2c_master_transmit(s_dev, w, sizeof w, 50) == ESP_OK;
}

esp_err_t touch_init(void)
{
    /* Reset first, while nothing is talking to it: the chip is awake for a
       while afterwards, which is when the auto-sleep write has to land. */
    claim_int(WATCH_TP_INT);
    pulse_reset(WATCH_TP_RST);

    if (i2cbus_init(I2CBUS_MAIN) != ESP_OK) return ESP_ERR_NOT_FOUND;
    bool answered = false;
    for (int i = 0; i < 5 && !answered; i++) {
        answered = i2cbus_probe(I2CBUS_MAIN, ADDR);
        if (!answered) vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!answered && WATCH_TP_INT < 0) {
        ESP_LOGW(TAG, "no CST816D touch at 0x%02x", ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    if (!answered)
        ESP_LOGW(TAG, "CST816D silent at 0x%02x (asleep?); reading it when INT "
                      "says a finger landed", ADDR);
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADDR,
        .scl_speed_hz = 100000,     /* a shared bus, with the IMU and RTC on it */
    };
    if (i2c_master_bus_add_device(i2cbus_handle(I2CBUS_MAIN), &dev, &s_dev) != ESP_OK)
        return ESP_ERR_NOT_FOUND;
    s_present = true;
    s_start_us = esp_timer_get_time();

    s_awake = answered && disable_auto_sleep();

    /* The id is logged rather than checked: the S, T, D and CST820 share this
       map and report 0xB4, 0xB5, 0xB6 and 0xB7, and refusing an unknown one
       would reject a working controller. A failed read is not fatal either --
       the address ACKed, and touch_debug will say it gave no data, which is
       the question worth seeing on the panel if it ever happens. */
    if (answered && !read_regs(REG_CHIP_ID, &s_chip_id, 1))
        ESP_LOGW(TAG, "CST816D at 0x%02x ACKs but its id read failed", ADDR);
    ESP_LOGI(TAG, "CST816D touch at 0x%02x, id 0x%02x, auto-sleep %s", ADDR,
             s_chip_id, s_awake ? "off" : "STILL ON");
    return ESP_OK;
}

/* One point. True if a finger is down, with raw controller coordinates out. */
static bool read_point(int *rx, int *ry)
{
    uint8_t d[6] = { 0 };
    if (!read_regs(REG_DATA, d, sizeof d)) return false;

    /* The first good read after a failed auto-sleep write is the chip awake,
       so try again then -- a few times, not every frame: a chip that answers
       reads but refuses the write would otherwise log an I2C error per poll. */
    if (!s_awake && s_wake_tries < 3) { s_wake_tries++; s_awake = disable_auto_sleep(); }

    /* d[0] is the chip's own gesture guess. It is ignored in favour of the
       state machine below, so every board's swipe feels the same. And some
       parts read 0xFF here with auto-sleep off and no finger (SensorLib says
       so of the CST816T): 0xFF & 0x0F is 15, which the range check drops. */
    uint8_t points = d[1] & 0x0F;
    if (points == 0 || points > 1) return false;   /* no valid touch */
    *rx = ((d[2] & 0x0F) << 8) | d[3];
    *ry = ((d[4] & 0x0F) << 8) | d[5];
    return true;
}

/* Reads the controller and advances the gesture. touch_tapped drives it. */
static void poll(void)
{
    if (esp_timer_get_time() - s_start_us < 1500000) { s_down = false; return; }

    int rx, ry;
    /* Asleep, it is only worth asking after INT has fallen or while a finger
       is known to be down; awake (auto-sleep off), every frame. */
    bool ask = s_present && (s_awake || s_int_seen || s_down);
    s_int_seen = false;
    bool down = ask && read_point(&rx, &ry);
    if (down) {
        s_raw_x = rx; s_raw_y = ry;
        /* Native portrait on both sides: no swap, no mirror. */
        int cx = rx;
        int cy = ry;
        if (cx < 0) cx = 0;
        if (cx >= CANVAS_W) cx = CANVAS_W - 1;
        if (cy < 0) cy = 0;
        if (cy >= CANVAS_H) cy = CANVAS_H - 1;
        if (!s_down) { s_down = true; s_down_x = cx; s_down_y = cy; }
        s_last_x = cx; s_last_y = cy;
    } else if (s_down) {
        s_down = false;
        int dx = s_last_x - s_down_x;
        int dy = s_last_y - s_down_y;
        if (abs(dx) >= SWIPE_MIN_PX && abs(dx) > abs(dy)) {
            s_pending_swipe = dx > 0 ? +1 : -1;
        } else if (abs(dx) < TAP_MAX_PX && abs(dy) < TAP_MAX_PX) {
            s_pending_tap = true;
            s_tap_x = s_last_x; s_tap_y = s_last_y;
        }
    }
}

bool touch_tapped(void)
{
    poll();
    bool t = s_pending_tap;
    s_pending_tap = false;
    return t;
}

int touch_swipe(void)
{
    int s = s_pending_swipe;
    s_pending_swipe = 0;
    return s;
}

void touch_point(int *x, int *y) { *x = s_tap_x; *y = s_tap_y; }
void touch_raw(int *x, int *y)   { *x = s_raw_x; *y = s_raw_y; }

/*
 * Whether the chip has ever returned data, as distinct from ACKing its
 * address. Not in touch.h, which every touch driver implements: the
 * AXS5106L's failure was exactly an ACK with no data behind it, and this is
 * the question to ask of a new controller on the bench.
 */
bool cst816d_touch_answered(void) { return s_answered; }

const char *touch_debug(void)
{
    static char buf[56];        /* room for three worst-case ints */
    if (!s_present)
        snprintf(buf, sizeof buf, "no touch at 0x%02x", ADDR);
    else if (!s_answered)
        snprintf(buf, sizeof buf, "touch 0x%02x acks, no data", ADDR);
    else
        snprintf(buf, sizeof buf, "touch %02x raw %d,%d e%d", s_chip_id,
                 s_raw_x, s_raw_y, s_read_errs);
    return buf;
}

#endif /* ESP_PLATFORM */
