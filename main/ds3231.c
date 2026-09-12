#include "ds3231.h"

#include <stdio.h>

#define SECS_PER_DAY 86400u

static uint8_t bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static int unbcd(uint8_t b)
{
    return ((b >> 4) & 0x0F) * 10 + (b & 0x0F);
}

bool ds3231_date_valid(const ds3231_date_t *d)
{
    if (d == NULL) return false;
    /* The chip holds two digits and a century bit, so it can express 1900 to
       2099. The floor is 2020 rather than 1900 because the only reason a real
       board reports an old date is that nothing ever set it. */
    return d->year >= 2020 && d->year <= 2099
        && d->month >= 1 && d->month <= 12
        && d->day >= 1 && d->day <= 31
        && d->wday >= 1 && d->wday <= 7;
}

void ds3231_format_date(const ds3231_date_t *d, char *out, int n)
{
    static const char *days[7] =
        { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    static const char *months[12] =
        { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

    if (n <= 0) return;
    if (!ds3231_date_valid(d)) { out[0] = '\0'; return; }
    snprintf(out, (size_t)n, "%s %d %s %d", days[d->wday - 1], d->day,
             months[d->month - 1], d->year);
}

void ds3231_pack(uint32_t secs, const ds3231_date_t *date, uint8_t regs[7])
{
    secs %= SECS_PER_DAY;
    regs[0] = bcd((int)(secs % 60));
    regs[1] = bcd((int)((secs / 60) % 60));
    regs[2] = bcd((int)(secs / 3600));      /* bit 6 clear: 24-hour mode */

    if (ds3231_date_valid(date)) {
        regs[3] = (uint8_t)date->wday;
        regs[4] = bcd(date->day);
        /* Bit 7 of the month is the century. Everything this board will see
           is 20xx, so it is always set. */
        regs[5] = (uint8_t)(bcd(date->month) | 0x80);
        regs[6] = bcd(date->year % 100);
        return;
    }
    /* No date to keep, but the chip still needs something valid to roll over
       at midnight without complaint: Saturday 1 January 2000. */
    regs[3] = 7;
    regs[4] = bcd(1);
    regs[5] = bcd(1);
    regs[6] = bcd(0);
}

bool ds3231_unpack(const uint8_t regs[7], uint32_t *secs, ds3231_date_t *date)
{
    /* Bit 7 of seconds is DS1307's clock-halt flag; masked here so the
       arithmetic is right either way, and irrelevant on a DS3231. */
    int s = unbcd(regs[0] & 0x7F);
    int m = unbcd(regs[1] & 0x7F);
    int h;
    if (regs[2] & 0x40) {
        /* 12-hour mode: bit 5 is PM, hours 1..12. */
        h = unbcd(regs[2] & 0x1F) % 12;
        if (regs[2] & 0x20) h += 12;
    } else {
        h = unbcd(regs[2] & 0x3F);
    }
    if (s > 59 || m > 59 || h > 23) return false;
    /* BCD digits above 9 mean garbage, not a time. */
    if ((regs[0] & 0x0F) > 9 || (regs[1] & 0x0F) > 9 || (regs[2] & 0x0F) > 9)
        return false;
    *secs = (uint32_t)(h * 3600 + m * 60 + s);

    if (date != NULL) {
        date->wday = regs[3] & 0x07;
        date->day = unbcd(regs[4] & 0x3F);
        date->month = unbcd(regs[5] & 0x1F);
        /* Century bit set means 20xx, clear means 19xx -- and a chip that
           has never been given a real date reads as 2000, which
           ds3231_date_valid rejects on the year floor. */
        date->year = ((regs[5] & 0x80) ? 2000 : 1900) + unbcd(regs[6]);
        if (!ds3231_date_valid(date)) date->year = 0;
    }
    return true;
}

#ifdef ESP_PLATFORM
#include "i2cbus.h"
#include "esp_log.h"

#define ADDR        0x68
#define REG_TIME    0x00
#define REG_STATUS  0x0F
#define REG_AGING   0x10
#define REG_TEMP    0x11       /* integer degrees, then quarters in bits 7:6 */
#define OSF         0x80       /* oscillator stop flag: time is not trusted */

static const char *TAG = "rtc";
static i2c_master_dev_handle_t s_dev;
static bool s_present;
static i2cbus_id_t s_bus = I2CBUS_MAIN;

esp_err_t ds3231_init(void)
{
    /*
     * Look on both buses. Where the chip lives is a question about the board,
     * not about the driver: the CrowPanel's shares the touch bus it was
     * wired alongside, while a board whose own I2C pins are not brought out
     * can only take one on the header. Bring each bus up rather than assume
     * someone else has -- this runs on every board, including ones where no
     * other driver touches I2C at all.
     */
    for (i2cbus_id_t which = I2CBUS_MAIN; which < I2CBUS_COUNT; which++) {
        if (i2cbus_init(which) != ESP_OK) continue;   /* no such bus here */
        if (!i2cbus_probe(which, ADDR)) continue;

        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ADDR,
            .scl_speed_hz = 100000,
        };
        esp_err_t err = i2c_master_bus_add_device(i2cbus_handle(which), &dev, &s_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "attach failed: %s", esp_err_to_name(err));
            return err;
        }
        s_present = true;
        s_bus = which;
        ESP_LOGI(TAG, "DS3231 answered at 0x%02X on the %s bus", ADDR,
                 which == I2CBUS_MAIN ? "main" : "aux");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "no DS3231 at 0x%02X on any bus; the clock waits for a Mac", ADDR);
    return ESP_ERR_NOT_FOUND;
}

static bool read_regs(uint8_t first, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &first, 1, out, n, 50) == ESP_OK;
}

i2cbus_id_t ds3231_bus(void) { return s_bus; }

bool ds3231_temperature(float *celsius)
{
    if (!s_present) return false;
    uint8_t t[2];
    if (!read_regs(REG_TEMP, t, sizeof t)) return false;
    /* Signed whole degrees, then two bits of quarter degree. */
    *celsius = (float)(int8_t)t[0] + (float)(t[1] >> 6) * 0.25f;
    return true;
}

bool ds3231_aging(int8_t *offset)
{
    if (!s_present) return false;
    uint8_t v;
    if (!read_regs(REG_AGING, &v, 1)) return false;
    *offset = (int8_t)v;
    return true;
}

bool ds3231_stopped(bool *stopped)
{
    if (!s_present) return false;
    uint8_t status;
    if (!read_regs(REG_STATUS, &status, 1)) return false;
    *stopped = (status & OSF) != 0;
    return true;
}

bool ds3231_read(uint32_t *secs)
{
    if (!s_present) return false;
    uint8_t status;
    if (!read_regs(REG_STATUS, &status, 1)) return false;
    if (status & OSF) {
        ESP_LOGW(TAG, "oscillator stopped since last set; battery flat or new chip");
        return false;
    }
    uint8_t regs[7];
    if (!read_regs(REG_TIME, regs, sizeof regs)) return false;
    if (!ds3231_unpack(regs, secs, NULL)) {
        ESP_LOGW(TAG, "registers are not a time: %02X %02X %02X",
                 regs[0], regs[1], regs[2]);
        return false;
    }
    return true;
}

bool ds3231_read_date(ds3231_date_t *date)
{
    if (!s_present) return false;
    uint8_t status;
    if (!read_regs(REG_STATUS, &status, 1)) return false;
    if (status & OSF) return false;        /* the chip lost track; do not believe it */
    uint8_t regs[7];
    if (!read_regs(REG_TIME, regs, sizeof regs)) return false;
    uint32_t secs;
    if (!ds3231_unpack(regs, &secs, date)) return false;
    return ds3231_date_valid(date);
}

bool ds3231_write(uint32_t secs, const ds3231_date_t *date)
{
    if (!s_present) return false;
    uint8_t buf[8];
    buf[0] = REG_TIME;
    ds3231_pack(secs, date, buf + 1);
    if (i2c_master_transmit(s_dev, buf, sizeof buf, 50) != ESP_OK) {
        ESP_LOGE(TAG, "write failed");
        return false;
    }
    /* Clear the stop flag, so the next boot trusts what was just set. */
    uint8_t status;
    if (read_regs(REG_STATUS, &status, 1) && (status & OSF)) {
        uint8_t clear[2] = { REG_STATUS, (uint8_t)(status & ~OSF) };
        i2c_master_transmit(s_dev, clear, sizeof clear, 50);
    }
    return true;
}
#endif
