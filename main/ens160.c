#include "ens160.h"

ens160_validity_t ens160_validity(uint8_t status)
{
    return (ens160_validity_t)((status >> 2) & 0x03);
}

bool ens160_has_new_data(uint8_t status)
{
    return (status & ENS160_STATUS_NEWDAT) != 0;
}

uint16_t ens160_encode_temp(float celsius)
{
    /*
     * Kelvin times sixty-four. Rounded, where ScioSense's own driver
     * truncates: a count is 1/64 K, so the two differ by at most sixteen
     * thousandths of a degree, which is far below anything the compensation
     * acts on -- but it is a real difference from the reference and is
     * asserted in the tests rather than left to be discovered.
     *
     * Below absolute zero is not a temperature, and the encoding is
     * unsigned, so the floor stops a wild reading wrapping round into a
     * plausible warm one.
     */
    float kelvin = celsius + 273.15f;
    if (kelvin < 0.0f) kelvin = 0.0f;
    float v = kelvin * 64.0f;
    if (v > 65535.0f) v = 65535.0f;
    return (uint16_t)(v + 0.5f);
}

uint16_t ens160_encode_rh(float humidity)
{
    if (humidity < 0.0f) humidity = 0.0f;
    if (humidity > 100.0f) humidity = 100.0f;
    return (uint16_t)(humidity * 512.0f + 0.5f);
}

const char *ens160_aqi_name(uint8_t aqi)
{
    /* The UBA index, which is a German federal standard rather than a scale
       this driver invented. One is best. */
    switch (aqi) {
    case 1:  return "excellent";
    case 2:  return "good";
    case 3:  return "moderate";
    case 4:  return "poor";
    case 5:  return "unhealthy";
    default: return "--";
    }
}

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ens160";
static i2c_master_dev_handle_t s_dev;
static i2cbus_id_t s_bus;
static bool s_present;
static uint16_t s_part_id;

static bool read_regs(uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, n, 100) == ESP_OK;
}

static bool write_regs(uint8_t reg, const uint8_t *v, size_t n)
{
    uint8_t buf[8];
    if (n + 1 > sizeof buf) return false;
    buf[0] = reg;
    for (size_t i = 0; i < n; i++) buf[i + 1] = v[i];
    return i2c_master_transmit(s_dev, buf, n + 1, 100) == ESP_OK;
}

static bool write_reg(uint8_t reg, uint8_t v)
{
    return write_regs(reg, &v, 1);
}

static bool attach(i2cbus_id_t which, uint8_t addr)
{
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(i2cbus_handle(which), &dev, &s_dev) == ESP_OK;
}

esp_err_t ens160_init(void)
{
    const uint8_t addrs[2] = { ENS160_ADDR_LOW, ENS160_ADDR_HIGH };
    s_present = false;

    for (i2cbus_id_t which = I2CBUS_MAIN; which < I2CBUS_COUNT; which++) {
        if (i2cbus_init(which) != ESP_OK) continue;
        for (int i = 0; i < 2; i++) {
            if (!i2cbus_probe(which, addrs[i])) continue;
            if (!attach(which, addrs[i])) continue;

            /* Something answered; the part ID says whether it is this. Two
               other chips live nearby on these modules and an address that
               merely acknowledges proves nothing. */
            uint8_t id[2];
            if (!read_regs(ENS160_REG_PART_ID, id, sizeof id)) {
                i2c_master_bus_rm_device(s_dev);
                continue;
            }
            uint16_t part = (uint16_t)(id[0] | (id[1] << 8));
            if (part != ENS160_PART_ID && part != ENS161_PART_ID) {
                ESP_LOGW(TAG, "0x%02X answered but reports part 0x%04X", addrs[i], part);
                i2c_master_bus_rm_device(s_dev);
                continue;
            }

            /* Idle first, then standard: the datasheet wants configuration
               done from idle, and coming from an unknown state is exactly
               what a fresh boot is. */
            write_reg(ENS160_REG_OPMODE, ENS160_OPMODE_IDLE);
            vTaskDelay(pdMS_TO_TICKS(10));
            if (!write_reg(ENS160_REG_OPMODE, ENS160_OPMODE_STANDARD)) {
                i2c_master_bus_rm_device(s_dev);
                continue;
            }

            s_bus = which;
            s_part_id = part;
            s_present = true;
            ESP_LOGI(TAG, "ENS160 at 0x%02X on the %s bus, part 0x%04X",
                     addrs[i], which == I2CBUS_MAIN ? "main" : "aux", part);
            ESP_LOGW(TAG, "readings settle over minutes; an hour from cold");
            return ESP_OK;
        }
    }
    ESP_LOGW(TAG, "no ENS160 at 0x52 or 0x53 on any bus");
    return ESP_ERR_NOT_FOUND;
}

bool ens160_present(void) { return s_present; }
i2cbus_id_t ens160_bus(void) { return s_bus; }
uint16_t ens160_part_id(void) { return s_part_id; }

bool ens160_compensate(float celsius, float humidity)
{
    if (!s_present) return false;
    uint16_t t = ens160_encode_temp(celsius);
    uint16_t h = ens160_encode_rh(humidity);
    /* Both registers are contiguous, so one write of four bytes. */
    const uint8_t v[4] = { (uint8_t)t, (uint8_t)(t >> 8),
                           (uint8_t)h, (uint8_t)(h >> 8) };
    return write_regs(ENS160_REG_TEMP_IN, v, sizeof v);
}

bool ens160_read(uint16_t *eco2_ppm, uint16_t *tvoc_ppb, uint8_t *aqi,
                 ens160_validity_t *validity)
{
    if (!s_present) return false;

    uint8_t status;
    if (!read_regs(ENS160_REG_DATA_STATUS, &status, 1)) return false;
    if (validity) *validity = ens160_validity(status);
    if (!ens160_has_new_data(status)) return false;

    /* AQI, TVOC and eCO2 sit in five consecutive registers from 0x21, so one
       read rather than three. */
    uint8_t d[5];
    if (!read_regs(ENS160_REG_DATA_AQI, d, sizeof d)) return false;
    if (aqi) *aqi = (uint8_t)(d[0] & 0x07);
    if (tvoc_ppb) *tvoc_ppb = (uint16_t)(d[1] | (d[2] << 8));
    if (eco2_ppm) *eco2_ppm = (uint16_t)(d[3] | (d[4] << 8));
    return true;
}
#endif
