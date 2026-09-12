#include "qmi8658.h"

#include "i2cbus.h"
#include "esp_log.h"

#include <stdio.h>

/* SA0 low gives 0x6A, high 0x6B. Which one a breakout uses depends on its
   pull, so probe both, as gt911.c does for the same reason. */
#define ADDR_A 0x6A
#define ADDR_B 0x6B

#define REG_WHOAMI   0x00
#define REG_REVISION 0x01
#define REG_CTRL1    0x02
#define REG_CTRL2    0x03      /* accelerometer range and rate */
#define REG_CTRL3    0x04      /* gyroscope range and rate */
#define REG_CTRL7    0x08      /* enable bits */
#define REG_AX_L     0x35      /* twelve bytes: ax ay az gx gy gz, LE int16 */

#define WHOAMI_QMI8658 0x05

/* CTRL2 0x13: accelerometer +/-4 g at 235 Hz. CTRL3 0x43: gyroscope
   +/-512 dps at 235 Hz. Those ranges set the scale factors below. */
#define CTRL2_VALUE 0x13
#define CTRL3_VALUE 0x43
#define ACCEL_LSB_PER_G   8192.0f    /* 32768 / 4 */
#define GYRO_LSB_PER_DPS    64.0f    /* 32768 / 512 */

static const char *TAG = "qmi8658";
static i2c_master_dev_handle_t s_dev;
static bool s_present;
static uint8_t s_addr;
static int s_read_errs;
static char s_dbg[80];

/* The last argument is milliseconds, not ticks. Passing pdMS_TO_TICKS here
   yields 2, which the driver rounds to a zero-tick wait, so every
   transaction returns ESP_ERR_INVALID_STATE without waiting. */
static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 50);
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t out[2] = { reg, value };
    return i2c_master_transmit(s_dev, out, sizeof out, 50);
}

esp_err_t qmi8658_init(void)
{
    esp_err_t err = i2cbus_init(I2CBUS_MAIN);
    if (err != ESP_OK) return err;

    /* If an address nothing should answer also ACKs, SDA is stuck low and
       "found" means nothing. */
    if (i2cbus_probe(I2CBUS_MAIN, 0x33)) {
        ESP_LOGE(TAG, "bogus address 0x33 answered; the bus is stuck");
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t addrs[2] = { ADDR_A, ADDR_B };
    for (int i = 0; i < 2; i++) {
        if (!i2cbus_probe(I2CBUS_MAIN, addrs[i])) continue;

        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(i2cbus_handle(I2CBUS_MAIN), &dev, &s_dev) != ESP_OK)
            continue;

        /* One byte at a time: auto-increment is a CTRL1 bit not yet written,
           so a two-byte read here may return the same register twice. */
        uint8_t who = 0, rev = 0;
        if (read_regs(REG_WHOAMI, &who, 1) != ESP_OK || who != WHOAMI_QMI8658) {
            ESP_LOGW(TAG, "0x%02X answered but WHO_AM_I is 0x%02X", addrs[i], who);
            i2c_master_bus_rm_device(s_dev);
            s_dev = NULL;
            continue;
        }
        read_regs(REG_REVISION, &rev, 1);

        s_addr = addrs[i];

        /* Address auto-increment, so the twelve data bytes come out in one
           transaction rather than twelve. */
        if (write_reg(REG_CTRL1, 0x40) != ESP_OK) return ESP_FAIL;
        if (write_reg(REG_CTRL2, CTRL2_VALUE) != ESP_OK) return ESP_FAIL;
        if (write_reg(REG_CTRL3, CTRL3_VALUE) != ESP_OK) return ESP_FAIL;
        if (write_reg(REG_CTRL7, 0x03) != ESP_OK) return ESP_FAIL;  /* aEN|gEN */

        s_present = true;
        ESP_LOGI(TAG, "QMI8658 at 0x%02X, revision 0x%02X", s_addr, rev);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "no QMI8658 at 0x%02X or 0x%02X; continuing without it",
             ADDR_A, ADDR_B);
    return ESP_ERR_NOT_FOUND;
}

bool qmi8658_present(void) { return s_present; }

esp_err_t qmi8658_read(qmi8658_sample_t *out)
{
    if (!s_present) return ESP_ERR_INVALID_STATE;

    uint8_t b[12];
    esp_err_t err = read_regs(REG_AX_L, b, sizeof b);
    if (err != ESP_OK) { s_read_errs++; return err; }

    int16_t raw[6];
    for (int i = 0; i < 6; i++)
        raw[i] = (int16_t)((uint16_t)b[2 * i] | ((uint16_t)b[2 * i + 1] << 8));

    out->ax = raw[0] / ACCEL_LSB_PER_G;
    out->ay = raw[1] / ACCEL_LSB_PER_G;
    out->az = raw[2] / ACCEL_LSB_PER_G;
    out->gx = raw[3] / GYRO_LSB_PER_DPS;
    out->gy = raw[4] / GYRO_LSB_PER_DPS;
    out->gz = raw[5] / GYRO_LSB_PER_DPS;
    return ESP_OK;
}

const char *qmi8658_debug(void)
{
    qmi8658_sample_t s;
    if (!s_present) {
        snprintf(s_dbg, sizeof s_dbg, "no imu");
    } else if (qmi8658_read(&s) != ESP_OK) {
        snprintf(s_dbg, sizeof s_dbg, "0x%02X errs=%d", s_addr, s_read_errs);
    } else {
        snprintf(s_dbg, sizeof s_dbg, "%+.2f %+.2f %+.2f", s.ax, s.ay, s.az);
    }
    return s_dbg;
}
