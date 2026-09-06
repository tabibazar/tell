#include "gt911.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define PIN_SCL 20
#define PIN_SDA 19

/* Which address answers depends on how RST floats at power-on, and RST is not
   wired on this board, so probe both. */
#define ADDR_A 0x5D
#define ADDR_B 0x14

#define REG_STATUS 0x814E

static const char *TAG = "gt911";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_present;
static bool s_was_down;

static esp_err_t read_status(uint8_t *status)
{
    uint8_t reg[2] = { REG_STATUS >> 8, REG_STATUS & 0xFF };
    return i2c_master_transmit_receive(s_dev, reg, sizeof reg, status, 1,
                                       pdMS_TO_TICKS(20));
}

static void clear_status(void)
{
    uint8_t clear[3] = { REG_STATUS >> 8, REG_STATUS & 0xFF, 0x00 };
    i2c_master_transmit(s_dev, clear, sizeof clear, pdMS_TO_TICKS(20));
}

esp_err_t gt911_init(void)
{
    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &s_bus));

    const uint8_t addrs[2] = { ADDR_A, ADDR_B };
    for (int i = 0; i < 2; i++) {
        if (i2c_master_probe(s_bus, addrs[i], 50) != ESP_OK) continue;
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = 400000,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev, &s_dev));
        s_present = true;
        ESP_LOGI(TAG, "GT911 answered at 0x%02X", addrs[i]);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "no GT911 at 0x%02X or 0x%02X; continuing without touch",
             ADDR_A, ADDR_B);
    return ESP_ERR_NOT_FOUND;
}

bool gt911_tapped(void)
{
    if (!s_present) return false;

    uint8_t status = 0;
    if (read_status(&status) != ESP_OK) return false;

    bool down = (status & 0x80) && (status & 0x0F) > 0;
    if (status & 0x80) clear_status();   /* the controller latches until cleared */

    /* Fire on release, so a resting finger does not cycle pages. */
    bool tapped = s_was_down && !down;
    s_was_down = down;
    return tapped;
}
