/*
 * envo's touch: the AXS5106L capacitive controller at 0x63 on the main I2C bus
 * (the same bus as the sensor array). A different chip from envio's AXS15231B,
 * with its own register read -- so it is its own driver, not axs_touch.c.
 *
 * The chip ACKs its address from power-up but, as first written (no reset,
 * one repeated-start read), NACKed every data read. Two changes together fixed
 * it, both as Waveshare's own code does: RST (GPIO47) pulsed at init, and the
 * register write and read as two transactions. Which of the two is actually
 * needed was not isolated. Polling only: INT (GPIO48) is not wired up.
 * touch_swipe/touch_tapped drive the read.
 *
 * The controller reports in the panel's native 172x320 portrait frame. The
 * display is driven landscape (swap_xy + mirror), so raw x/y are swapped into
 * the 320x172 canvas here. A horizontal swipe pages the env views. If a swipe
 * ever goes the wrong way, or a tap lands mirrored, flip the sign where cx/cy
 * are formed -- it cannot be calibrated without the glass in hand.
 *
 * The read (register 0x01, 14 bytes; points in byte 1, point 1 at bytes 2-5)
 * is Waveshare's own, from esp_lcd_touch_axs5106 in their ESP-IDF BSP.
 */
#include "touch.h"

#include "i2cbus.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <stdlib.h>

#define ADDR      0x63
#define PIN_RST   47        /* Waveshare BSP: TP_RST 47, TP_INT 48 */
#define CANVAS_W  320
#define CANVAS_H  172

/* A drag at least this far across is a swipe; a press that moves less is a
   tap. In canvas pixels. */
#define SWIPE_MIN_PX 60
#define TAP_MAX_PX   25

static const char *TAG = "touch";
static i2c_master_dev_handle_t s_dev;
static bool s_present;

/* Gesture state, in canvas coordinates. */
static bool s_down;
static int  s_down_x, s_down_y, s_last_x, s_last_y;
static int  s_raw_x, s_raw_y;
static int  s_pending_swipe;    /* -1 left, +1 right, 0 none */
static bool s_pending_tap;
static int  s_tap_x, s_tap_y;
static int64_t s_start_us;      /* boot grace against a power-up phantom touch */

esp_err_t touch_init(void)
{
    /* Reset first, as Waveshare's init does. */
    gpio_config_t rst = { .pin_bit_mask = 1ULL << PIN_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rst);
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    if (i2cbus_init(I2CBUS_MAIN) != ESP_OK) return ESP_ERR_NOT_FOUND;
    if (!i2cbus_probe(I2CBUS_MAIN, ADDR)) {
        ESP_LOGW(TAG, "no AXS5106L touch at 0x%02x", ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADDR,
        .scl_speed_hz = 100000,     /* a shared bus with the sensors on it */
    };
    if (i2c_master_bus_add_device(i2cbus_handle(I2CBUS_MAIN), &dev, &s_dev) != ESP_OK)
        return ESP_ERR_NOT_FOUND;
    s_present = true;
    s_start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "AXS5106L touch at 0x%02x", ADDR);
    return ESP_OK;
}

/* One point. True if a finger is down, with raw controller coordinates out. */
static bool read_point(int *rx, int *ry)
{
    uint8_t reg = 0x01;
    uint8_t d[14] = { 0 };
    /* Two transactions with a stop between, as Waveshare's read does. */
    esp_err_t err = i2c_master_transmit(s_dev, &reg, 1, 50);
    if (err == ESP_OK) err = i2c_master_receive(s_dev, d, sizeof d, 50);
    if (err != ESP_OK)
        return false;
    uint8_t points = d[1] & 0x0F;
    if (points == 0 || points > 2) return false;   /* no valid touch */
    *rx = ((d[2] & 0x0F) << 8) | d[3];
    *ry = ((d[4] & 0x0F) << 8) | d[5];
    return true;
}

/* Reads the controller and advances the gesture. touch_tapped drives it. */
static void poll(void)
{
    if (esp_timer_get_time() - s_start_us < 1500000) { s_down = false; return; }

    int rx, ry;
    bool down = s_present && read_point(&rx, &ry);
    if (down) {
        s_raw_x = rx; s_raw_y = ry;
        /* Native portrait -> landscape canvas: the long axis (0..319) is the
           canvas x, the short axis (0..171) is the canvas y. */
        int cx = ry;
        int cy = rx;
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

const char *touch_debug(void)
{
    static char buf[40];
    snprintf(buf, sizeof buf, s_present ? "touch 0x%02x raw %d,%d" : "no touch",
             ADDR, s_raw_x, s_raw_y);
    return buf;
}
