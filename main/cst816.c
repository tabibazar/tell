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
 * Controller coordinates to canvas coordinates.
 *
 * The glass is 240x320 upright. The panel is driven turned on its side and
 * mirrored top to bottom, and only 168 of the controller's 240 rows are ours
 * -- the rest is the letterbox margin. So a touch has to make the same three
 * journeys the pixels do, in reverse:
 *
 *   turned:    the controller's long axis is the canvas's x
 *   mirrored:  the canvas's y runs the other way from the controller's x
 *   inset:     the margin above our rows has to come off
 *
 * Wrong signs here put taps in the wrong corner rather than nowhere, which is
 * why the raw and the mapped pair are both logged: the answer is settled by
 * touching a known corner and reading what came out, not by reasoning.
 */
#define CST816_NATIVE_W 240     /* the glass, upright */
#define CST816_GAP_Y    36      /* the letterbox margin, as display_st7789.c */

static void to_canvas(int raw_x, int raw_y, int *cx, int *cy)
{
    *cx = raw_y;
    *cy = (CST816_NATIVE_W - 1 - raw_x) - CST816_GAP_Y;
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

const char *touch_debug(void) { return s_debug; }
