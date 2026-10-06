/*
 * panel1's GT911, polled: its INT line is on the expander, not a GPIO, and
 * a swipe needs where the finger is all along, not just where it lifted.
 * lcd_init has already reset it with INT low, which puts it at 0x5D.
 */
#include "touch.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

#define REG_STATUS 0x814E
#define REG_POINT1 0x8150

i2c_master_bus_handle_t lcd_i2c(void);

static const char *TAG = "touch";
static i2c_master_dev_handle_t s_dev;
static bool s_down;
static int s_x, s_y;

static esp_err_t rd(uint16_t reg, uint8_t *buf, size_t n)
{
    uint8_t r[2] = { reg >> 8, reg & 0xFF };
    return i2c_master_transmit_receive(s_dev, r, 2, buf, n, 30);
}

static esp_err_t wr8(uint16_t reg, uint8_t v)
{
    uint8_t b[3] = { reg >> 8, reg & 0xFF, v };
    return i2c_master_transmit(s_dev, b, 3, 30);
}

esp_err_t touch_init(void)
{
    i2c_master_bus_handle_t bus = lcd_i2c();
    if (!bus) return ESP_ERR_INVALID_STATE;
    const uint8_t addrs[] = { 0x5D, 0x14 };
    for (size_t i = 0; i < sizeof addrs; i++) {
        if (i2c_master_probe(bus, addrs[i], 50) != ESP_OK) continue;
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addrs[i], .scl_speed_hz = 400000 };
        if (i2c_master_bus_add_device(bus, &dc, &s_dev) != ESP_OK) return ESP_FAIL;
        uint8_t id[4] = { 0 };
        rd(0x8140, id, 4);
        ESP_LOGI(TAG, "GT%.4s at 0x%02X", (const char *)id, addrs[i]);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "GT911 not found");
    return ESP_ERR_NOT_FOUND;
}

bool touch_read(int *x, int *y)
{
    uint8_t st;
    if (!s_dev || rd(REG_STATUS, &st, 1) != ESP_OK) return s_down = false;
    if (st & 0x80) {
        int n = st & 0x0F;
        if (n > 0) {
            uint8_t p[4];
            if (rd(REG_POINT1, p, 4) == ESP_OK) {
                s_x = p[0] | (p[1] << 8);
                s_y = p[2] | (p[3] << 8);
                s_down = true;
            }
        } else {
            s_down = false;
        }
        wr8(REG_STATUS, 0);
    }
    /* Not ready: no new report this time, so the finger is where it was. */
    *x = s_x;
    *y = s_y;
    return s_down;
}
