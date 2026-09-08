#include "bmp280.h"

#include "i2cbus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* SDO low gives 0x76, high 0x77. This board answers on 0x77, but probe both
   so a differently strapped module still works. */
#define ADDR_A 0x76
#define ADDR_B 0x77

#define REG_ID        0xD0
#define REG_CTRL_MEAS 0xF4
#define REG_CONFIG    0xF5
#define REG_PRESS_MSB 0xF7      /* six bytes: press[3], temp[3] */

/* Oversampling x1 on both, normal mode: the lowest setting, because we are
   after the noise rather than an accurate reading, and it converts fastest. */
#define CTRL_MEAS_NORMAL 0x27

static const char *TAG = "bmp280";
static i2c_master_dev_handle_t s_dev;
static bool s_present;
static uint8_t s_id;

/* Milliseconds, not ticks -- see the note in qmi8658.c. */
static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 50);
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t out[2] = { reg, value };
    return i2c_master_transmit(s_dev, out, sizeof out, 50);
}

esp_err_t bmp280_init(void)
{
    esp_err_t err = i2cbus_init();
    if (err != ESP_OK) return err;

    const uint8_t addrs[2] = { ADDR_A, ADDR_B };
    for (int i = 0; i < 2; i++) {
        if (!i2cbus_probe(addrs[i])) continue;

        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(i2cbus_handle(), &dev, &s_dev) != ESP_OK)
            continue;

        uint8_t id = 0;
        if (read_regs(REG_ID, &id, 1) != ESP_OK
            || (id != 0x58 && id != 0x60 && id != 0x61)) {
            ESP_LOGW(TAG, "0x%02X answered but its id is 0x%02X, not a "
                          "BMP280 family part", addrs[i], id);
            i2c_master_bus_rm_device(s_dev);
            s_dev = NULL;
            continue;
        }

        s_id = id;
        write_reg(REG_CONFIG, 0x00);          /* no filter: we want the noise */
        if (write_reg(REG_CTRL_MEAS, CTRL_MEAS_NORMAL) != ESP_OK) return ESP_FAIL;
        s_present = true;
        ESP_LOGI(TAG, "sensor at 0x%02X, id 0x%02X", addrs[i], s_id);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "no BMP280 family part on the bus");
    return ESP_ERR_NOT_FOUND;
}

bool bmp280_present(void) { return s_present; }
uint8_t bmp280_id(void) { return s_id; }

bool bmp280_raw(uint32_t *temperature, uint32_t *pressure)
{
    if (!s_present) return false;
    uint8_t b[6];
    if (read_regs(REG_PRESS_MSB, b, sizeof b) != ESP_OK) return false;
    *pressure    = ((uint32_t)b[0] << 12) | ((uint32_t)b[1] << 4) | (b[2] >> 4);
    *temperature = ((uint32_t)b[3] << 12) | ((uint32_t)b[4] << 4) | (b[5] >> 4);
    return true;
}

uint32_t bmp280_entropy(void)
{
    if (!s_present) return 0;

    uint32_t acc = 0;
    int got = 0;
    for (int i = 0; i < 12; i++) {
        uint32_t t, p;
        if (!bmp280_raw(&t, &p)) break;
        /* Shift off the bottom four bits first. At x1 oversampling the chip
           does not fill its xlsb byte, so those bits are always zero -- mixed
           in as they are, they pin the low nibble of the seed to zero and
           four bits of it are dead. The real noise starts above them.

           Only the low end carries anything anyway: the top of a temperature
           reading is the room, which is the same every morning. */
        uint32_t v = ((t >> 4) & 0xFFFF) | (((p >> 4) & 0xFFFF) << 16);
        acc ^= v;
        acc *= 2654435761u;             /* avalanche, so one bit moves many */
        acc ^= acc >> 15;
        got++;
        /* Normal mode at x1 finishes a conversion in about 6 ms; anything
           faster just reads the same sample twice. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (got == 0) return 0;
    ESP_LOGI(TAG, "seed 0x%08X from %d readings", (unsigned)acc, got);
    return acc;
}
