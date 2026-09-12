#include "ds3231.h"

#define SECS_PER_DAY 86400u

static uint8_t bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static int unbcd(uint8_t b)
{
    return ((b >> 4) & 0x0F) * 10 + (b & 0x0F);
}

void ds3231_pack(uint32_t secs, uint8_t regs[7])
{
    secs %= SECS_PER_DAY;
    regs[0] = bcd((int)(secs % 60));
    regs[1] = bcd((int)((secs / 60) % 60));
    regs[2] = bcd((int)(secs / 3600));      /* bit 6 clear: 24-hour mode */
    /* The date is never read back, but the chip needs something valid to
       roll over at midnight without complaint: Saturday 1 January 2000. */
    regs[3] = 7;
    regs[4] = bcd(1);
    regs[5] = bcd(1);
    regs[6] = bcd(0);
}

bool ds3231_unpack(const uint8_t regs[7], uint32_t *secs)
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
    return true;
}

#ifdef ESP_PLATFORM
#include "i2cbus.h"
#include "esp_log.h"

#define ADDR        0x68
#define REG_TIME    0x00
#define REG_STATUS  0x0F
#define OSF         0x80       /* oscillator stop flag: time is not trusted */

static const char *TAG = "rtc";
static i2c_master_dev_handle_t s_dev;
static bool s_present;

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
    if (!ds3231_unpack(regs, secs)) {
        ESP_LOGW(TAG, "registers are not a time: %02X %02X %02X",
                 regs[0], regs[1], regs[2]);
        return false;
    }
    return true;
}

bool ds3231_write(uint32_t secs)
{
    if (!s_present) return false;
    uint8_t buf[8];
    buf[0] = REG_TIME;
    ds3231_pack(secs, buf + 1);
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
