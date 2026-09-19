/*
 * AXP2101 rail bring-up for envio. See axp2101.h for why this exists at all.
 *
 * The register map and the voltages come from Waveshare's factory firmware
 * (ESP-IDF/01_factory bsp_axp2101) read against the X-Powers AXP2101 datasheet,
 * not from live hardware -- so the exact rail feeding the panel is inferred,
 * not measured. Every value here is a 3.3 V or lower rail that is in spec for
 * this board's parts; the risk of a wrong guess is a panel that stays dark,
 * which is why the backlight was deliberately left on a plain GPIO where it
 * cannot be masked by a PMIC mistake. Confirm the rails at first light and
 * trim this to only what the panel and touch actually need.
 */
#include "axp2101.h"

#include "i2cbus.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

#include <string.h>

#define AXP2101_ADDR 0x34

/* Registers, from the AXP2101 datasheet. */
#define REG_DC_ONOFF   0x80   /* DC1..DC5 enable, bits 0..4 */
#define REG_DC2_VOL    0x83
#define REG_DC3_VOL    0x84
#define REG_DC4_VOL    0x85
#define REG_DC5_VOL    0x86   /* top 3 bits reserved, low 5 are the voltage */
#define REG_LDO_ONOFF0 0x90   /* ALDO1-4 b0-3, BLDO1-2 b4-5, CPUSLDO b6, DLDO1 b7 */
#define REG_LDO_ONOFF1 0x91   /* DLDO2 b0 */
#define REG_ALDO1_VOL  0x92
#define REG_ALDO2_VOL  0x93
#define REG_ALDO3_VOL  0x94
#define REG_ALDO4_VOL  0x95
#define REG_BLDO1_VOL  0x96
#define REG_BLDO2_VOL  0x97
#define REG_CPUSLDO_VOL 0x98  /* top 3 bits reserved, low 5 are the voltage */
#define REG_DLDO1_VOL  0x99
#define REG_DLDO2_VOL  0x9A

/* Battery gauge and status, for axp2101_battery(). */
#define REG_STATUS0    0x00   /* power-path status; bit5 = VBUS good */
#define REG_STATUS1    0x01   /* charger status; bits[6:5] = charge state */
#define REG_ADC_EN     0x30   /* ADC channel enables; bit0 = battery voltage */
#define REG_GAUGE_EN   0x68   /* battery detection / fuel gauge enable */
#define REG_BATT_V_H   0x34   /* battery voltage, high 6 bits */
#define REG_BATT_V_L   0x35   /* battery voltage, low 8 bits */
#define REG_BATT_PCT   0xA4   /* fuel gauge percent, 0-100 (0xFF = no reading) */

/*
 * Voltage encodings, per the datasheet's per-rail tables:
 *   LDO family (ALDO/BLDO/DLDO): 0.5-3.5 V in 100 mV steps, code = (mV-500)/100.
 *   DC2/DC4: 0.5-1.2 V in 10 mV steps, code = (mV-500)/10.
 *   DC3: 1.6-3.4 V band is 100 mV steps from code 88, so 3.3 V = 88 + 17 = 105.
 * These three cover every rail Waveshare's firmware sets a voltage for.
 */
#define LDO_MV(mv)  (uint8_t)(((mv) - 500) / 100)
#define DC_LO_MV(mv) (uint8_t)(((mv) - 500) / 10)
#define DC3_3V3     0x69   /* 3.3 V, see above */
/* DC5 is its own table: 1.4-3.7 V in 100 mV steps, code = (mV-1400)/100, so
   3.3 V = 19. CPUSLDO is 0.5-1.4 V in 50 mV steps, code = (mV-500)/50, so
   1.0 V = 10. Both keep the register's top 3 bits, so set them read-modified. */
#define DC5_3V3      0x13
#define CPUSLDO_1V0  0x0A

static const char *TAG = "axp2101";
static i2c_master_dev_handle_t s_dev;

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof buf, 50);
}

static esp_err_t rd(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 50);
}

/* Writes a 5-bit voltage code into a register whose top 3 bits are reserved,
   preserving those bits. Used for DC5 and CPUSLDO. */
static esp_err_t set_vol5(uint8_t reg, uint8_t code)
{
    uint8_t cur = 0;
    if (rd(reg, &cur) != ESP_OK) return ESP_FAIL;
    return wr(reg, (uint8_t)((cur & 0xE0) | (code & 0x1F)));
}

esp_err_t axp2101_init(void)
{
    esp_err_t err = i2cbus_init(I2CBUS_MAIN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no I2C bus for the PMIC");
        return err;
    }

    /* add_device does not touch the bus, so it "succeeds" with nothing there
       and every later write is silently dropped -- which looks exactly like an
       unlit panel. Probe first, so a missing PMIC fails here with a clear
       message rather than three chips downstream. */
    if (!i2cbus_probe(I2CBUS_MAIN, AXP2101_ADDR)) {
        ESP_LOGE(TAG, "no PMIC answering at 0x%02x on SDA%d/SCL%d",
                 AXP2101_ADDR, i2cbus_sda(I2CBUS_MAIN), i2cbus_scl(I2CBUS_MAIN));
        return ESP_ERR_NOT_FOUND;
    }

    if (s_dev == NULL) {
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = AXP2101_ADDR,
            .scl_speed_hz = 400000,
        };
        err = i2c_master_bus_add_device(i2cbus_handle(I2CBUS_MAIN), &dev, &s_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "could not attach the PMIC at 0x%02x", AXP2101_ADDR);
            return err;
        }
    }

    /* Rail voltages, matching the factory firmware. The LDOs at 3.3 V are the
       likely home of the panel and touch supplies; DC3 at 3.3 V is the other
       candidate, so it is set explicitly rather than left at its reset value.
       Every rail the factory firmware sets a voltage for is set here too, so
       none is enabled below at an unknown reset default. */
    wr(REG_ALDO1_VOL, LDO_MV(3300));
    wr(REG_ALDO2_VOL, LDO_MV(3300));
    wr(REG_ALDO3_VOL, LDO_MV(3300));
    wr(REG_ALDO4_VOL, LDO_MV(3300));
    wr(REG_BLDO1_VOL, LDO_MV(1500));
    wr(REG_BLDO2_VOL, LDO_MV(2800));
    wr(REG_DLDO1_VOL, LDO_MV(3300));
    wr(REG_DLDO2_VOL, LDO_MV(3300));
    wr(REG_DC2_VOL, DC_LO_MV(1000));
    wr(REG_DC3_VOL, DC3_3V3);
    wr(REG_DC4_VOL, DC_LO_MV(1000));
    /* DC5 and CPUSLDO share their register with reserved top bits, so
       read-modify-write the low 5. */
    set_vol5(REG_DC5_VOL, DC5_3V3);
    set_vol5(REG_CPUSLDO_VOL, CPUSLDO_1V0);

    /* Enable the LDO rails: ALDO1-4, BLDO1-2, CPUSLDO, DLDO1 (0x90) and
       DLDO2 (0x91). These masks are exactly the enable bits, so a plain write
       is right; nothing else lives in these registers. */
    esp_err_t e = wr(REG_LDO_ONOFF0, 0xFF);
    e |= wr(REG_LDO_ONOFF1, 0x01);

    /* Enable DC2..DC5 without disturbing DC1, which is the 3.3 V system rail
       the SoC is already running from -- read, set bits 1..4, write back. */
    uint8_t dc = 0;
    if (rd(REG_DC_ONOFF, &dc) == ESP_OK) {
        e |= wr(REG_DC_ONOFF, dc | 0x1E);
    } else {
        e |= wr(REG_DC_ONOFF, 0x1F);   /* could not read; force DC1..DC5 on */
    }

    if (e != ESP_OK) {
        ESP_LOGE(TAG, "PMIC ack'd but a rail write failed; panel may stay dark");
        return ESP_FAIL;
    }

    /* Battery voltage ADC and fuel gauge, for axp2101_battery(). Waveshare's
       factory firmware may already have these on; setting the enable bits
       again is harmless either way. Not fatal if they fail -- a board with no
       cell wired reads "no answer" from the gauge regardless. */
    uint8_t adc = 0;
    if (rd(REG_ADC_EN, &adc) == ESP_OK) wr(REG_ADC_EN, adc | 0x01);
    else wr(REG_ADC_EN, 0x01);
    wr(REG_GAUGE_EN, 0x01);

    ESP_LOGI(TAG, "PMIC rails up (0x%02x)", AXP2101_ADDR);
    return ESP_OK;
}

bool axp2101_battery(axp2101_batt_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof *out);

    uint8_t status0 = 0, status1 = 0, vh = 0, vl = 0, pct = 0;
    if (rd(REG_STATUS0, &status0) != ESP_OK) return false;
    if (rd(REG_STATUS1, &status1) != ESP_OK) return false;
    if (rd(REG_BATT_V_H, &vh) != ESP_OK) return false;
    if (rd(REG_BATT_V_L, &vl) != ESP_OK) return false;
    if (rd(REG_BATT_PCT, &pct) != ESP_OK) return false;

    out->percent = (pct <= 100) ? (int)pct : -1;
    out->millivolts = (int)(((vh & 0x3F) << 8) | vl);
    out->vbus = (status0 & 0x20) != 0;

    uint8_t chg = (status1 >> 5) & 0x03;
    if (chg == 0x01) out->charge = AXP_CHG_CHARGING;
    else if (chg == 0x02) out->charge = AXP_CHG_DISCHARGING;
    else out->charge = AXP_CHG_STANDBY;

    return true;
}
