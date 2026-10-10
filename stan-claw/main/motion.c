#include "motion.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "lcd.h"

static const char *TAG = "motion";

#define REG_WHOAMI 0x00
#define REG_CTRL1  0x02      /* register address auto-increment */
#define REG_CTRL2  0x03      /* accelerometer range and rate */
#define REG_CTRL7  0x08      /* enable bits */
#define REG_AX_L   0x35
#define WHOAMI     0x05
#define LSB_PER_G  8192.0f   /* +-4 g, CTRL2 0x13 as main/qmi8658.c sets it */

static i2c_master_dev_handle_t s_dev;

static esp_err_t wr(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = { reg, v };
    return i2c_master_transmit(s_dev, b, 2, 50);
}

esp_err_t motion_init(void)
{
    const uint8_t addrs[] = { 0x6B, 0x6A };
    for (size_t i = 0; i < sizeof addrs; i++) {
        if (i2c_master_probe(lcd_i2c(), addrs[i], 50) != ESP_OK) continue;
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addrs[i], .scl_speed_hz = 400000 };
        if (i2c_master_bus_add_device(lcd_i2c(), &dc, &s_dev) != ESP_OK) return ESP_FAIL;
        uint8_t reg = REG_WHOAMI, who = 0;
        if (i2c_master_transmit_receive(s_dev, &reg, 1, &who, 1, 50) != ESP_OK || who != WHOAMI) {
            ESP_LOGW(TAG, "0x%02X answers but is not a QMI8658 (0x%02X)", addrs[i], who);
            s_dev = NULL;
            return ESP_ERR_NOT_FOUND;
        }
        if (wr(REG_CTRL1, 0x40) != ESP_OK || wr(REG_CTRL2, 0x13) != ESP_OK || wr(REG_CTRL7, 0x01) != ESP_OK) {
            s_dev = NULL;
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "QMI8658 at 0x%02X: knocks wake the screen", addrs[i]);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "no QMI8658: knocks will not wake the screen");
    return ESP_ERR_NOT_FOUND;
}

bool motion_read(float *ax, float *ay, float *az)
{
    if (!s_dev) return false;
    uint8_t reg = REG_AX_L, b[6];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, b, sizeof b, 50) != ESP_OK) return false;
    *ax = (int16_t)(b[0] | b[1] << 8) / LSB_PER_G;
    *ay = (int16_t)(b[2] | b[3] << 8) / LSB_PER_G;
    *az = (int16_t)(b[4] | b[5] << 8) / LSB_PER_G;
    return true;
}
