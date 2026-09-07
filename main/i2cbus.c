#include "i2cbus.h"

#include "esp_log.h"

/* Shared by the GT911 touch controller and the DS3231 clock. */
#define PIN_SCL 20
#define PIN_SDA 19

static const char *TAG = "i2cbus";
static i2c_master_bus_handle_t s_bus;

esp_err_t i2cbus_init(void)
{
    if (s_bus != NULL) return ESP_OK;

    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus init failed: %s", esp_err_to_name(err));
        s_bus = NULL;
        return err;
    }
    ESP_LOGI(TAG, "bus up on SDA %d / SCL %d", PIN_SDA, PIN_SCL);
    return ESP_OK;
}

i2c_master_bus_handle_t i2cbus_handle(void)
{
    return s_bus;
}

bool i2cbus_probe(uint8_t addr)
{
    if (s_bus == NULL) return false;
    return i2c_master_probe(s_bus, addr, 100) == ESP_OK;
}
