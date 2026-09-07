#include "gt911.h"

#include "i2cbus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include <stdio.h>

/* Which address answers depends on how RST floats at power-on, and RST is not
   wired on this board, so probe both. */
#define ADDR_A 0x5D
#define ADDR_B 0x14

#define REG_STATUS 0x814E
#define REG_POINT1 0x8150

static const char *TAG = "gt911";
static i2c_master_dev_handle_t s_dev;
static bool s_present;
static int s_x, s_y;          /* where the last press was seen */
static bool s_was_down;

/* Instrumentation, because the serial console on this board is unreadable. */
static uint8_t s_addr;
static uint8_t s_last_status;
static int s_reads_ok;
static int s_read_errs;
static int s_downs;
static int s_taps;
static int s_probe_a = -1, s_probe_b = -1, s_probe_bogus = -1;
static esp_err_t s_last_read_err = ESP_OK;
static esp_err_t s_bus_err = ESP_OK;
static char s_dbg[80];

static esp_err_t read_status(uint8_t *status)
{
    uint8_t reg[2] = { REG_STATUS >> 8, REG_STATUS & 0xFF };
    /* The last argument is milliseconds, not ticks. Passing pdMS_TO_TICKS
       here yielded 2, which the driver rounded to a zero-tick wait, so every
       transaction returned ESP_ERR_INVALID_STATE without waiting. */
    return i2c_master_transmit_receive(s_dev, reg, sizeof reg, status, 1, 50);
}

/* Point 1 is seven bytes at 0x8150: track id, x low/high, y low/high,
   size low/high. */
static void read_point(void)
{
    uint8_t reg[2] = { REG_POINT1 >> 8, REG_POINT1 & 0xFF };
    uint8_t buf[7] = {0};
    if (i2c_master_transmit_receive(s_dev, reg, sizeof reg, buf, sizeof buf,
                                    50) != ESP_OK)
        return;
    s_x = buf[1] | (buf[2] << 8);
    s_y = buf[3] | (buf[4] << 8);
}

void gt911_point(int *x, int *y)
{
    *x = s_x;
    *y = s_y;
}

static void clear_status(void)
{
    uint8_t clear[3] = { REG_STATUS >> 8, REG_STATUS & 0xFF, 0x00 };
    i2c_master_transmit(s_dev, clear, sizeof clear, 50);
}

esp_err_t gt911_init(void)
{
    /* The bus belongs to i2cbus, so the clock can share it. */
    s_bus_err = i2cbus_init();
    if (s_bus_err != ESP_OK) return s_bus_err;

    /* Probe every address, including one nothing should answer. If the bogus
       address also ACKs, SDA is stuck low and "found" means nothing. */
    s_probe_a = i2cbus_probe(ADDR_A) ? ESP_OK : ESP_ERR_NOT_FOUND;
    s_probe_b = i2cbus_probe(ADDR_B) ? ESP_OK : ESP_ERR_NOT_FOUND;
    s_probe_bogus = i2cbus_probe(0x33) ? ESP_OK : ESP_ERR_NOT_FOUND;

    const uint8_t addrs[2] = { ADDR_A, ADDR_B };
    for (int i = 0; i < 2; i++) {
        esp_err_t probe = (i == 0) ? s_probe_a : s_probe_b;
        if (probe != ESP_OK) continue;
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = 100000,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(i2cbus_handle(), &dev, &s_dev));
        s_present = true;
        s_addr = addrs[i];
        ESP_LOGI(TAG, "GT911 answered at 0x%02X", addrs[i]);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "no GT911 at 0x%02X or 0x%02X; continuing without touch",
             ADDR_A, ADDR_B);
    return ESP_ERR_NOT_FOUND;
}

bool gt911_tapped(void)
{
    if (!s_present) return false;

    uint8_t status = 0;
    esp_err_t err = read_status(&status);
    if (err != ESP_OK) { s_read_errs++; s_last_read_err = err; return false; }
    s_reads_ok++;
    s_last_status = status;

    bool down = (status & 0x80) && (status & 0x0F) > 0;
    if (down) { s_downs++; read_point(); }
    if (status & 0x80) clear_status();   /* the controller latches until cleared */

    /* Fire on release, so a resting finger does not cycle pages. */
    bool tapped = s_was_down && !down;
    s_was_down = down;
    if (tapped) s_taps++;
    return tapped;
}

const char *gt911_debug(void)
{
    if (s_bus_err != ESP_OK) {
        snprintf(s_dbg, sizeof s_dbg, "i2c bus err %d", (int)s_bus_err);
    } else if (!s_present) {
        snprintf(s_dbg, sizeof s_dbg, "no gt911: 5D=%d 14=%d bogus33=%d",
                 s_probe_a, s_probe_b, s_probe_bogus);
    } else {
        /* esp_err_t names: 0x107 TIMEOUT, 0x105 NOT_FOUND, 0xffff FAIL(NAK). */
        snprintf(s_dbg, sizeof s_dbg,
                 "touch x=%d y=%d   taps=%d", s_x, s_y, s_taps);
    }
    return s_dbg;
}
