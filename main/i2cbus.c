#include "i2cbus.h"

#include "esp_log.h"

#include <stdio.h>
#include "sdkconfig.h"

/* Board dependent: the CrowPanel wires its GT911 to GPIO19/20, the Feather's
   STEMMA QT port is GPIO42/41, wave's QMI8658 is on GPIO48/47. The auxiliary
   pins are header pins and are -1 on boards that expose none. Defaults live
   in Kconfig.projbuild. */
static const struct {
    const char *name;
    int sda, scl;
    i2c_port_num_t port;
} s_buses[I2CBUS_COUNT] = {
    [I2CBUS_MAIN] = { "main", CONFIG_SCREEN_I2C_SDA, CONFIG_SCREEN_I2C_SCL, I2C_NUM_0 },
    [I2CBUS_AUX]  = { "aux",  CONFIG_SCREEN_I2C_AUX_SDA, CONFIG_SCREEN_I2C_AUX_SCL, I2C_NUM_1 },
};

static const char *TAG = "i2cbus";
static i2c_master_bus_handle_t s_bus[I2CBUS_COUNT];

/*
 * Says what is actually on a bus, once. A driver that does not find its chip
 * can only report its own absence; it cannot tell you whether nothing is
 * wired, something is wired to the wrong pins, or the right chip is answering
 * on an address you did not expect. This can, and it costs about twenty
 * milliseconds at boot.
 */
static void scan(i2cbus_id_t which)
{
    char list[96];
    size_t len = 0;
    int n = 0;
    list[0] = '\0';
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (i2c_master_probe(s_bus[which], a, 50) != ESP_OK) continue;
        n++;
        if (len + 6 < sizeof list)
            len += (size_t)snprintf(list + len, sizeof list - len, " 0x%02X", a);
    }
    ESP_LOGI(TAG, "%s: %d device(s):%s", s_buses[which].name, n, n ? list : " none");
}

esp_err_t i2cbus_init(i2cbus_id_t which)
{
    if (which >= I2CBUS_COUNT) return ESP_ERR_INVALID_ARG;
    if (s_bus[which] != NULL) return ESP_OK;
    /* A board with no pins for this bus has no bus, which is not a failure:
       the caller simply does without, as it does without the chip. */
    if (s_buses[which].sda < 0 || s_buses[which].scl < 0) return ESP_ERR_NOT_SUPPORTED;

    i2c_master_bus_config_t cfg = {
        .i2c_port = s_buses[which].port,
        .sda_io_num = s_buses[which].sda,
        .scl_io_num = s_buses[which].scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus[which]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s bus init failed: %s", s_buses[which].name,
                 esp_err_to_name(err));
        s_bus[which] = NULL;
        return err;
    }
    ESP_LOGI(TAG, "%s bus up on SDA %d / SCL %d", s_buses[which].name,
             s_buses[which].sda, s_buses[which].scl);
    scan(which);
    return ESP_OK;
}

i2c_master_bus_handle_t i2cbus_handle(i2cbus_id_t which)
{
    return which < I2CBUS_COUNT ? s_bus[which] : NULL;
}

int i2cbus_sda(i2cbus_id_t which)
{
    return which < I2CBUS_COUNT ? s_buses[which].sda : -1;
}

int i2cbus_scl(i2cbus_id_t which)
{
    return which < I2CBUS_COUNT ? s_buses[which].scl : -1;
}

bool i2cbus_probe(i2cbus_id_t which, uint8_t addr)
{
    if (which >= I2CBUS_COUNT || s_bus[which] == NULL) return false;
    return i2c_master_probe(s_bus[which], addr, 100) == ESP_OK;
}
