/*
 * visio's touch: the AXS15231B's touch half, read over I2C at 0x3B on the
 * shared bus. It is the same chip as the display; only the touch side is here.
 *
 * The controller reports in the panel's native 320x480 portrait frame, which
 * is also how the display is driven (native portrait), so a touch at (x, y)
 * sits under canvas (x, y) directly. A horizontal swipe is a change in x; if a
 * swipe ever goes the wrong way, flip the sign where s_pending_swipe is set.
 *
 * The read command and the parse are Waveshare's own (ESP-IDF/01_factory
 * bsp_touch): write an 11-byte request, read 14 bytes back.
 */
#include "touch.h"

#include "i2cbus.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

#include <stdlib.h>

#define ADDR      0x3B
#define CANVAS_W  320
#define CANVAS_H  480

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

esp_err_t touch_init(void)
{
    if (i2cbus_init(I2CBUS_MAIN) != ESP_OK) return ESP_ERR_NOT_FOUND;
    if (!i2cbus_probe(I2CBUS_MAIN, ADDR)) {
        ESP_LOGW(TAG, "no AXS15231B touch at 0x%02x", ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADDR,
        .scl_speed_hz = 100000,     /* the busy shared bus, like the QMI */
    };
    if (i2c_master_bus_add_device(i2cbus_handle(I2CBUS_MAIN), &dev, &s_dev) != ESP_OK)
        return ESP_ERR_NOT_FOUND;
    s_present = true;
    ESP_LOGI(TAG, "AXS15231B touch at 0x%02x", ADDR);
    return ESP_OK;
}

/* One point. True if a finger is down, with raw controller coordinates out. */
static bool read_point(int *rx, int *ry)
{
    static const uint8_t cmd[11] =
        { 0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00 };
    uint8_t d[14] = { 0 };
    if (i2c_master_transmit_receive(s_dev, cmd, sizeof cmd, d, sizeof d, 50) != ESP_OK)
        return false;
    if (d[0] == 0xff || d[1] == 0 || d[1] > 2) return false;   /* no valid touch */
    *rx = ((d[2] & 0x0F) << 8) | d[3];
    *ry = ((d[4] & 0x0F) << 8) | d[5];
    return true;
}

/* Reads the controller and advances the gesture. touch_tapped drives it. */
static void poll(void)
{
    int rx, ry;
    bool down = s_present && read_point(&rx, &ry);
    if (down) {
        s_raw_x = rx; s_raw_y = ry;
        int cx = rx;                    /* native portrait: straight through */
        int cy = ry;
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
