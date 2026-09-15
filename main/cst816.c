#include "touch.h"

#include "esp_log.h"
#include "i2cbus.h"

#include <stdio.h>

/*
 * A Hynitron CST816D, the capacitive controller on envo's 2.8" panel.
 *
 * Register map from Espressif's own esp_lcd_touch_cst816s, which is the
 * authority worth trusting here: the vendor datasheet is a compressed PDF and
 * the part is sold under four names -- CST816S, T, D and CST820 -- that share
 * this layout and differ only in the chip ID they report.
 *
 * Reading five bytes from 0x02 gives the finger count and one point:
 *
 *   [0] number of fingers, 0 or 1
 *   [1] X high, in the LOW nibble
 *   [2] X low
 *   [3] Y high, in the LOW nibble
 *   [4] Y low
 *
 * The high nibbles carry flags on some firmwares, which is why only the low
 * four bits are taken. Masking them off is not optional tidiness: with a flag
 * set, an unmasked high byte puts the touch several thousand pixels away.
 */

/* The canvas this board draws on, as display_st7789.c letterboxes it. */
#define CST816_CANVAS_W   320
#define CST816_CANVAS_H   168

#define CST816_ADDR       0x15
#define CST816_REG_DATA   0x02
#define CST816_REG_CHIPID 0xA7

static const char *TAG = "cst816";
static i2c_master_dev_handle_t s_dev;
static bool s_present;
static uint8_t s_chip_id;

/* The press being tracked, and the tap waiting to be collected. */
static bool s_down;
static int s_last_x, s_last_y;
static int s_raw_x, s_raw_y;
static bool s_pending;
static char s_debug[64];

static bool read_regs(uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, n, 100) == ESP_OK;
}

esp_err_t touch_init(void)
{
    s_present = false;
    snprintf(s_debug, sizeof s_debug, "no CST816 at 0x%02X", CST816_ADDR);

    for (i2cbus_id_t which = I2CBUS_MAIN; which < I2CBUS_COUNT; which++) {
        if (i2cbus_init(which) != ESP_OK) continue;
        if (!i2cbus_probe(which, CST816_ADDR)) continue;

        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = CST816_ADDR,
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(i2cbus_handle(which), &dev, &s_dev) != ESP_OK)
            continue;

        /* The chip ID is logged rather than checked against a list: the four
           parts that share this register map report four different values,
           and refusing an unknown one would reject a working controller. */
        if (!read_regs(CST816_REG_CHIPID, &s_chip_id, 1)) {
            i2c_master_bus_rm_device(s_dev);
            continue;
        }
        s_present = true;
        snprintf(s_debug, sizeof s_debug, "CST816 0x%02X id 0x%02X",
                 CST816_ADDR, s_chip_id);
        ESP_LOGI(TAG, "CST816 at 0x%02X on the %s bus, chip id 0x%02X",
                 CST816_ADDR, which == I2CBUS_MAIN ? "main" : "aux", s_chip_id);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "no CST816 at 0x%02X on any bus", CST816_ADDR);
    return ESP_ERR_NOT_FOUND;
}

/*
 * Controller coordinates to canvas coordinates, measured rather than derived.
 *
 * The first attempt worked this out from the display's own transform -- turn,
 * mirror, subtract the letterbox -- and was wrong, because the digitiser does
 * not share the panel's coordinate space. Touching four known crosses gave:
 *
 *   canvas  24, 24   controller   45, 312
 *   canvas 296, 24   controller   47,   5
 *   canvas  24,144   controller  212, 313
 *   canvas 296,144   controller  210,  12
 *
 * Which says three things at once. Canvas x comes from the controller's y and
 * runs backwards. Canvas y comes from the controller's x and runs forwards.
 * And neither is one-to-one: the y axis is scaled by about 0.73, so the
 * digitiser is reporting across a grid that is not the panel's -- common on
 * these modules, where the touch layer is configured for whatever resolution
 * the factory had to hand. No amount of reasoning about mirrors would have
 * produced 0.73; only touching the glass does.
 *
 * Kept as the measured endpoints rather than as pre-divided constants, so the
 * numbers in the code are the numbers that came off the board.
 */
#define RAW_Y_AT_LEFT    312     /* controller y where canvas x is 24 */
#define RAW_Y_AT_RIGHT     8     /*               ...and where it is 296 */
#define CANVAS_X_LEFT     24
#define CANVAS_X_RIGHT   296

#define RAW_X_AT_TOP      46     /* controller x where canvas y is 24 */
#define RAW_X_AT_BOTTOM  211     /*               ...and where it is 144 */
#define CANVAS_Y_TOP      24
#define CANVAS_Y_BOTTOM  144

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void to_canvas(int raw_x, int raw_y, int *cx, int *cy)
{
    int x = CANVAS_X_LEFT
          + (RAW_Y_AT_LEFT - raw_y) * (CANVAS_X_RIGHT - CANVAS_X_LEFT)
            / (RAW_Y_AT_LEFT - RAW_Y_AT_RIGHT);
    int y = CANVAS_Y_TOP
          + (raw_x - RAW_X_AT_TOP) * (CANVAS_Y_BOTTOM - CANVAS_Y_TOP)
            / (RAW_X_AT_BOTTOM - RAW_X_AT_TOP);

    /* A fingertip beyond the calibrated span still belongs to the nearest
       edge; letting it run off makes a tap on the rim hit nothing. */
    *cx = clamp(x, 0, CST816_CANVAS_W - 1);
    *cy = clamp(y, 0, CST816_CANVAS_H - 1);
}

bool touch_tapped(void)
{
    if (!s_present) return false;

    uint8_t d[5];
    if (!read_regs(CST816_REG_DATA, d, sizeof d)) return false;

    bool down = (d[0] & 0x0F) > 0;
    if (down) {
        int raw_x = ((int)(d[1] & 0x0F) << 8) | d[2];
        int raw_y = ((int)(d[3] & 0x0F) << 8) | d[4];
        s_raw_x = raw_x; s_raw_y = raw_y;
        to_canvas(raw_x, raw_y, &s_last_x, &s_last_y);
        snprintf(s_debug, sizeof s_debug, "raw %d,%d -> %d,%d",
                 raw_x, raw_y, s_last_x, s_last_y);
        if (!s_down) {
            s_down = true;
            s_pending = true;         /* a press began; report it on release */
        }
        return false;
    }

    /* Released. Report the tap once, the way the GT911 driver does, so a
       finger held down does not fire repeatedly. */
    if (s_down && s_pending) {
        s_down = false;
        s_pending = false;
        ESP_LOGI(TAG, "tap %s", s_debug);
        return true;
    }
    s_down = false;
    return false;
}

void touch_point(int *x, int *y)
{
    if (x) *x = s_last_x;
    if (y) *y = s_last_y;
}

void touch_raw(int *x, int *y)
{
    if (x) *x = s_raw_x;
    if (y) *y = s_raw_y;
}

const char *touch_debug(void) { return s_debug; }
