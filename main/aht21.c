#include "aht21.h"

uint8_t aht21_crc(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

void aht21_convert(const uint8_t d[6], float *celsius, float *humidity)
{
    /* Twenty bits each, sharing d[3]: humidity takes its high nibble and
       temperature its low one. Reading the shared byte the wrong way round
       is the mistake this driver exists to not make. */
    uint32_t raw_h = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4)
                   | ((uint32_t)d[3] >> 4);
    uint32_t raw_t = (((uint32_t)d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8)
                   | (uint32_t)d[5];

    /* Both are fractions of a full twenty-bit scale: humidity over 0..100,
       temperature over -50..150. */
    if (humidity) {
        float rh = (float)raw_h * 100.0f / 1048576.0f;
        if (rh < 0.0f) rh = 0.0f;
        if (rh > 100.0f) rh = 100.0f;
        *humidity = rh;
    }
    if (celsius) *celsius = (float)raw_t * 200.0f / 1048576.0f - 50.0f;
}

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "aht21";
static i2c_master_dev_handle_t s_dev;
static i2cbus_id_t s_bus;
static bool s_present;

static bool tx(const uint8_t *d, size_t n)
{
    return i2c_master_transmit(s_dev, d, n, 100) == ESP_OK;
}

static bool rx(uint8_t *d, size_t n)
{
    return i2c_master_receive(s_dev, d, n, 100) == ESP_OK;
}

esp_err_t aht21_init(void)
{
    s_present = false;
    for (i2cbus_id_t which = I2CBUS_MAIN; which < I2CBUS_COUNT; which++) {
        if (i2cbus_init(which) != ESP_OK) continue;
        if (!i2cbus_probe(which, AHT21_ADDR)) continue;

        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = AHT21_ADDR,
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(i2cbus_handle(which), &dev, &s_dev) != ESP_OK)
            continue;

        /* The chip calibrates itself at power-on and says so in its status.
           Only nudge it if it has not: an unnecessary init costs 10 ms and a
           needless write to a part that was already ready. */
        uint8_t st = 0;
        if (rx(&st, 1) && !(st & AHT21_STATUS_CAL)) {
            const uint8_t init[3] = { 0xBE, 0x08, 0x00 };
            tx(init, sizeof init);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        s_bus = which;
        s_present = true;
        ESP_LOGI(TAG, "AHT21 at 0x%02X on the %s bus", AHT21_ADDR,
                 which == I2CBUS_MAIN ? "main" : "aux");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "no AHT21 at 0x%02X on any bus", AHT21_ADDR);
    return ESP_ERR_NOT_FOUND;
}

bool aht21_present(void) { return s_present; }
i2cbus_id_t aht21_bus(void) { return s_bus; }

bool aht21_read(float *celsius, float *humidity)
{
    if (!s_present) return false;

    const uint8_t measure[3] = { 0xAC, 0x33, 0x00 };
    if (!tx(measure, sizeof measure)) return false;
    vTaskDelay(pdMS_TO_TICKS(80));      /* the chip's own conversion time */

    uint8_t d[7];
    if (!rx(d, sizeof d)) return false;
    if (d[0] & AHT21_STATUS_BUSY) return false;
    /* The CRC covers the status and both readings. A sensor that answers with
       the right number of wrong bytes is exactly what a CRC is for. */
    if (aht21_crc(d, 6) != d[6]) {
        ESP_LOGW(TAG, "bad CRC; reading discarded "
                 "(%02x %02x %02x %02x %02x %02x crc %02x want %02x)",
                 d[0], d[1], d[2], d[3], d[4], d[5], d[6], aht21_crc(d, 6));
        return false;
    }
    aht21_convert(d, celsius, humidity);
    return true;
}
#endif
