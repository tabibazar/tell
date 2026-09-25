#include "pcf85063.h"

#include <stddef.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#define SECS_PER_DAY 86400u

/* Control_1 (00h). Only CIE and CAP_SEL are ever kept; see ds3231_write. */
#define CTRL1_EXT_TEST 0x80    /* clocked from CLKOUT, not the crystal */
#define CTRL1_STOP     0x20    /* the time circuits are frozen */
#define CTRL1_CIE      0x04    /* correction interrupt enable */
#define CTRL1_12_24    0x02    /* 12-hour mode, which nothing here reads */
#define CTRL1_CAP_SEL  0x01    /* crystal load, 7 pF or 12.5 pF */

#define OS             0x80    /* seconds bit 7: the oscillator has stopped */

static uint8_t bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static int unbcd(uint8_t b)
{
    return ((b >> 4) & 0x0F) * 10 + (b & 0x0F);
}

/* Both digits 0..9. A nibble of 0xA..0xF is garbage, not a number, and
   unbcd would quietly turn it into one. */
static bool bcd_ok(uint8_t b)
{
    return (b & 0x0F) <= 9 && (b >> 4) <= 9;
}

/*
 * A date the chip can actually hold, which is stricter than ds3231_date_valid:
 * that allows the 31st of any month. Writing 30 February to the chip is not
 * defined, and reading it back means the registers are wrong. Every fourth year
 * is a leap year, 2000 included. That is the chip's own rule, and right for
 * its whole range of 2000 to 2099.
 */
static bool date_real(const ds3231_date_t *d)
{
    static const uint8_t days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (!ds3231_date_valid(d)) return false;
    int last = days[d->month - 1] + (d->month == 2 && d->year % 4 == 0);
    return d->day <= last;
}

bool pcf85063_pack(uint32_t secs, const ds3231_date_t *date, uint8_t regs[7])
{
    secs %= SECS_PER_DAY;
    regs[0] = bcd((int)(secs % 60));        /* bit 7, OS, clear */
    regs[1] = bcd((int)((secs / 60) % 60));
    regs[2] = bcd((int)(secs / 3600));      /* 24-hour; Control_1 says which */

    if (date_real(date)) {
        regs[3] = bcd(date->day);
        /* The chip counts weekdays 0..6 and wraps; which day is 0 is up to
           whoever sets it. Sunday, as the datasheet has it, so that its
           power-on Saturday 1 January 2000 (weekday 6) reads true, and Monday
           to Saturday keep their own numbers from date +%u. */
        regs[4] = (uint8_t)(date->wday % 7);
        regs[5] = bcd(date->month);
        regs[6] = bcd(date->year % 100);    /* no century bit: 20xx only */
        return true;
    }
    /* Nothing worth keeping, so the chip's own reset value: Saturday 1 January
       2000, which rolls over at midnight like any day and which a reader
       rejects on the year floor. */
    regs[3] = bcd(1);
    regs[4] = 6;
    regs[5] = bcd(1);
    regs[6] = bcd(0);
    return false;
}

bool pcf85063_unpack(const uint8_t regs[7], uint32_t *secs, ds3231_date_t *date)
{
    /* Bit 7 of the seconds is OS and bit 7 of the minutes is unused; the
       hours use bits 5:0 in 24-hour mode. Masked, so the arithmetic is right
       whatever the flag says. Whether to believe the time is
       pcf85063_untrusted's question, not this one's. */
    uint8_t s = regs[0] & 0x7F;
    uint8_t m = regs[1] & 0x7F;
    uint8_t h = regs[2] & 0x3F;
    if (!bcd_ok(s) || !bcd_ok(m) || !bcd_ok(h)) return false;
    if (unbcd(s) > 59 || unbcd(m) > 59 || unbcd(h) > 23) return false;
    *secs = (uint32_t)(unbcd(h) * 3600 + unbcd(m) * 60 + unbcd(s));

    if (date != NULL) {
        uint8_t d = regs[3] & 0x3F;
        uint8_t wd = regs[4] & 0x07;
        uint8_t mo = regs[5] & 0x1F;
        uint8_t y = regs[6];
        date->day = unbcd(d);
        date->wday = wd == 0 ? 7 : wd;      /* Sunday is 0 on the chip, 7 here */
        date->month = unbcd(mo);
        date->year = 2000 + unbcd(y);
        /* A chip nobody has set reads as 2000, which the year floor rejects;
           a weekday of 7 or a digit above nine means the registers are not a
           date at all. Either way the year goes to 0, as ds3231_unpack does
           it, so the date is plainly invalid rather than subtly wrong. */
        if (!bcd_ok(d) || !bcd_ok(mo) || !bcd_ok(y) || wd > 6 || !date_real(date))
            date->year = 0;
    }
    return true;
}

const char *pcf85063_untrusted(uint8_t control_1, const uint8_t regs[7])
{
    /* OS first: it is the one that means the time was lost, rather than that
       someone configured the chip oddly. It is set at power-up, so a new chip
       or one whose cell went flat says so here until the next write. */
    if (regs[0] & OS)
        return "oscillator stopped since last set; battery flat or new chip";
    if (control_1 & CTRL1_STOP)
        return "clock is stopped (STOP set); the time is frozen";
    if (control_1 & CTRL1_EXT_TEST)
        return "clock is in external test mode, not on its crystal";
    /* Twelve-hour mode puts AM/PM in bit 5 of the hours, which a 24-hour read
       would take for a tens digit: 1 PM as 21:00. Nothing here sets it, but
       the chip keeps whatever the last firmware did while its cell lasts. */
    if (control_1 & CTRL1_12_24)
        return "clock is in 12-hour mode; the next set puts it back to 24";
    return NULL;
}

/*
 * The hardware half is watch's alone. Every .c in main/ is compiled on every
 * board, so without this guard the ds3231_* names below would collide with
 * ds3231.c's own on a board that really has a DS3231. ds3231.c carries the
 * matching guard the other way round.
 */
#if defined(ESP_PLATFORM) && CONFIG_SCREEN_BOARD_TOUCH_LCD_169
#include "i2cbus.h"
#include "esp_log.h"

#define ADDR       0x51
#define REG_CTRL1  0x00
#define REG_TIME   0x04
#define RAW_LEN    (REG_TIME + 7)          /* Control_1 through Years */

static const char *TAG = "rtc";
static i2c_master_dev_handle_t s_dev;
static bool s_present;

esp_err_t ds3231_init(void)
{
    /* The chip is soldered to the main bus alongside the CST816D and the
       QMI8658, so there is no other bus to look on. Bring the bus up rather
       than assume the touch driver already has; the order in main.c is not
       this driver's business. */
    if (i2cbus_init(I2CBUS_MAIN) != ESP_OK || !i2cbus_probe(I2CBUS_MAIN, ADDR)) {
        ESP_LOGW(TAG, "no PCF85063 at 0x%02X; the clock waits for a Mac", ADDR);
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADDR,
        .scl_speed_hz = 100000,     /* a shared bus, with the touch on it */
    };
    esp_err_t err = i2c_master_bus_add_device(i2cbus_handle(I2CBUS_MAIN), &dev, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "attach failed: %s", esp_err_to_name(err));
        return err;
    }
    s_present = true;
    ESP_LOGI(TAG, "PCF85063 answered at 0x%02X on the main bus", ADDR);
    return ESP_OK;
}

static bool read_regs(uint8_t first, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &first, 1, out, n, 50) == ESP_OK;
}

/*
 * Control_1 through the year in one transaction. The flags that say whether
 * to believe the time come with the time itself, and main.c's drift sampling
 * polls this hard for a second or two at a time while it hunts the seconds
 * edge. So one transaction rather than two, on a bus the touch shares. The
 * chip holds its time counters still for the length of a read, so the seven
 * bytes are one instant and never a carry caught halfway.
 */
static bool read_raw(uint8_t raw[RAW_LEN])
{
    return read_regs(REG_CTRL1, raw, RAW_LEN);
}

i2cbus_id_t ds3231_bus(void) { return I2CBUS_MAIN; }

/* No thermometer: the PCF85063 is not temperature compensated, so there is
   no crystal reading to show. The RTC page says "unreadable". */
bool ds3231_temperature(float *celsius)
{
    (void)celsius;
    return false;
}

/* The chip does have a trim, the Offset register at 02h, but in steps of
   about 4.3 ppm rather than the DS3231's 0.1. Shown under the DS3231's label
   it would be a number in the wrong units, so it is not shown at all. */
bool ds3231_aging(int8_t *offset)
{
    (void)offset;
    return false;
}

bool ds3231_stopped(bool *stopped)
{
    if (!s_present) return false;
    uint8_t sec;
    if (!read_regs(REG_TIME, &sec, 1)) return false;
    *stopped = (sec & OS) != 0;
    return true;
}

bool ds3231_read(uint32_t *secs)
{
    if (!s_present) return false;
    uint8_t raw[RAW_LEN];
    if (!read_raw(raw)) return false;
    const uint8_t *regs = raw + REG_TIME;
    const char *why = pcf85063_untrusted(raw[REG_CTRL1], regs);
    if (why != NULL) {
        ESP_LOGW(TAG, "%s", why);
        return false;
    }
    if (!pcf85063_unpack(regs, secs, NULL)) {
        ESP_LOGW(TAG, "registers are not a time: %02X %02X %02X",
                 regs[0], regs[1], regs[2]);
        return false;
    }
    return true;
}

bool ds3231_read_date(ds3231_date_t *date)
{
    if (!s_present) return false;
    uint8_t raw[RAW_LEN];
    if (!read_raw(raw)) return false;
    /* Quietly, as ds3231.c does: ds3231_read has already said why. */
    if (pcf85063_untrusted(raw[REG_CTRL1], raw + REG_TIME) != NULL) return false;
    uint32_t secs;
    if (!pcf85063_unpack(raw + REG_TIME, &secs, date)) return false;
    return ds3231_date_valid(date);
}

bool ds3231_write(uint32_t secs, const ds3231_date_t *date)
{
    if (!s_present) return false;

    /*
     * Control_1 first, so the chip is running and in 24-hour mode before it is
     * given a 24-hour time. The other way round, a chip left in 12-hour mode
     * could tick once in between and carry 23:xx as if bit 5 meant PM. Only
     * CIE and CAP_SEL are carried over. The rest are cleared: EXT_TEST and
     * STOP so the crystal drives the count, 12_24 for 24-hour mode, and SR and
     * the unused bits because a software reset is a pattern written into this
     * very register. CAP_SEL is kept as found. Which load the fitted crystal
     * wants has not been checked on this board, and a guess that changes it
     * could only make the clock worse; if the RTC page shows tens of ppm, that
     * bit is the first suspect. Nothing is written when nothing needs to
     * change, which is every time but the first.
     */
    uint8_t c1;
    if (!read_regs(REG_CTRL1, &c1, 1)) {
        ESP_LOGE(TAG, "write failed: Control_1 unreadable");
        return false;
    }
    uint8_t want = c1 & (CTRL1_CIE | CTRL1_CAP_SEL);
    if (want != c1) {
        uint8_t fix[2] = { REG_CTRL1, want };
        if (i2c_master_transmit(s_dev, fix, sizeof fix, 50) != ESP_OK) {
            ESP_LOGE(TAG, "write failed: Control_1");
            return false;
        }
        ESP_LOGI(TAG, "Control_1 %02X -> %02X: running, 24-hour", c1, want);
    }

    /*
     * Writing the seconds clears OS. So the next boot trusts what was just
     * set, with no separate status write as the DS3231 needs. With no date to
     * give, only the time is written, 04h..06h. The chip's own calendar keeps
     * running, which is what ds3231.h promises and what main.c relies on when a
     * Mac syncs without sending a date.
     */
    uint8_t buf[1 + 7];
    buf[0] = REG_TIME;
    bool with_date = pcf85063_pack(secs, date, buf + 1);
    size_t n = with_date ? sizeof buf : 1 + 3;
    if (i2c_master_transmit(s_dev, buf, n, 50) != ESP_OK) {
        ESP_LOGE(TAG, "write failed");
        return false;
    }
    return true;
}
#endif
