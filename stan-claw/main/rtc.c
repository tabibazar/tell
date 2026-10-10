#include "rtc.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "lcd.h"

static const char *TAG = "rtc";
#define ADDR    0x51
#define REG_SEC 0x04         /* seconds, minutes, hours, days, weekdays, months, years */

static i2c_master_dev_handle_t s_dev;

static int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }
static uint8_t tobcd(int v) { return (uint8_t)((v / 10) << 4 | v % 10); }

/* Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm): newlib
   here has no timegm, and mktime would apply the local zone. */
static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

esp_err_t clockchip_init(void)
{
    if (i2c_master_probe(lcd_i2c(), ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "no PCF85063 at 0x%02X: deep sleep wakes on the ESP32's own timer", ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADDR, .scl_speed_hz = 400000 };
    return i2c_master_bus_add_device(lcd_i2c(), &dc, &s_dev);
}

bool clockchip_get(time_t *utc)
{
    if (!s_dev) return false;
    uint8_t reg = REG_SEC, b[7];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, b, sizeof b, 50) != ESP_OK) return false;
    if (b[0] & 0x80) return false;               /* OS: the oscillator stopped, the time is not good */
    int year = 2000 + bcd(b[6]);
    if (year < 2026) return false;               /* never set */
    long days = days_from_civil(year, bcd(b[5] & 0x1F), bcd(b[3] & 0x3F));
    *utc = (time_t)(days * 86400L + bcd(b[2] & 0x3F) * 3600L + bcd(b[1] & 0x7F) * 60L + bcd(b[0] & 0x7F));
    return true;
}

void clockchip_set(time_t utc)
{
    if (!s_dev) return;
    struct tm t;
    gmtime_r(&utc, &t);
    uint8_t b[8] = { REG_SEC, tobcd(t.tm_sec), tobcd(t.tm_min), tobcd(t.tm_hour), tobcd(t.tm_mday),
                     (uint8_t)t.tm_wday, tobcd(t.tm_mon + 1), tobcd(t.tm_year % 100) };
    if (i2c_master_transmit(s_dev, b, sizeof b, 50) == ESP_OK)
        ESP_LOGI(TAG, "set from the network: %04d-%02d-%02d %02d:%02d UTC", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
}
