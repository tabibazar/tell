#include "bme280.h"

/* ---- the arithmetic, which is the whole point ------------------------- */

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static int16_t  s16(const uint8_t *p) { return (int16_t)u16(p); }

/* Sign-extends a twelve-bit value, which is how H4 and H5 are stored. */
static int16_t s12(int v)
{
    return (int16_t)(v & 0x800 ? v - 0x1000 : v);
}

void bme280_parse_calib(const uint8_t tp[26], const uint8_t h[7],
                        bme280_cal_t *c)
{
    c->t1 = u16(tp + 0);
    c->t2 = s16(tp + 2);
    c->t3 = s16(tp + 4);
    c->p1 = u16(tp + 6);
    c->p2 = s16(tp + 8);
    c->p3 = s16(tp + 10);
    c->p4 = s16(tp + 12);
    c->p5 = s16(tp + 14);
    c->p6 = s16(tp + 16);
    c->p7 = s16(tp + 18);
    c->p8 = s16(tp + 20);
    c->p9 = s16(tp + 22);
    /* tp[24] is reserved; tp[25] is 0xA1, which is H1. */
    c->h1 = tp[25];

    c->h2 = s16(h + 0);                                   /* 0xE1, 0xE2 */
    c->h3 = h[2];                                         /* 0xE3 */
    /* 0xE4 is H4's high eight bits, 0xE5 holds H4's low nibble and H5's. */
    c->h4 = s12((h[3] << 4) | (h[4] & 0x0F));
    c->h5 = s12((h[5] << 4) | (h[4] >> 4));
    c->h6 = (int8_t)h[6];                                 /* 0xE7 */
    c->t_fine = 0;
}

void bme280_raw(const uint8_t d[8], int32_t *adc_p, int32_t *adc_t,
                int32_t *adc_h)
{
    /* 0xF7..0xF9 pressure, 0xFA..0xFC temperature, both twenty bits left
       aligned in three bytes; 0xFD..0xFE humidity, sixteen. */
    if (adc_p) *adc_p = (int32_t)(((uint32_t)d[0] << 12) | ((uint32_t)d[1] << 4)
                                  | ((uint32_t)d[2] >> 4));
    if (adc_t) *adc_t = (int32_t)(((uint32_t)d[3] << 12) | ((uint32_t)d[4] << 4)
                                  | ((uint32_t)d[5] >> 4));
    if (adc_h) *adc_h = (int32_t)(((uint32_t)d[6] << 8) | (uint32_t)d[7]);
}

float bme280_temperature(bme280_cal_t *c, int32_t adc_t)
{
    float v1 = ((float)adc_t / 16384.0f - (float)c->t1 / 1024.0f) * (float)c->t2;
    float d  = (float)adc_t / 131072.0f - (float)c->t1 / 8192.0f;
    float v2 = d * d * (float)c->t3;
    c->t_fine = (int32_t)(v1 + v2);
    return (v1 + v2) / 5120.0f;
}

float bme280_pressure(const bme280_cal_t *c, int32_t adc_p)
{
    float v1 = (float)c->t_fine / 2.0f - 64000.0f;
    float v2 = v1 * v1 * (float)c->p6 / 32768.0f;
    v2 = v2 + v1 * (float)c->p5 * 2.0f;
    v2 = v2 / 4.0f + (float)c->p4 * 65536.0f;
    v1 = ((float)c->p3 * v1 * v1 / 524288.0f + (float)c->p2 * v1) / 524288.0f;
    v1 = (1.0f + v1 / 32768.0f) * (float)c->p1;
    /* A zero here is a chip that gave nonsense calibration, not a pressure. */
    if (v1 == 0.0f) return 0.0f;

    float p = 1048576.0f - (float)adc_p;
    p = (p - v2 / 4096.0f) * 6250.0f / v1;
    v1 = (float)c->p9 * p * p / 2147483648.0f;
    v2 = p * (float)c->p8 / 32768.0f;
    p = p + (v1 + v2 + (float)c->p7) / 16.0f;
    return p / 100.0f;                     /* pascals to hectopascals */
}

float bme280_humidity(const bme280_cal_t *c, int32_t adc_h)
{
    float v = (float)c->t_fine - 76800.0f;
    v = ((float)adc_h - ((float)c->h4 * 64.0f + (float)c->h5 / 16384.0f * v))
        * ((float)c->h2 / 65536.0f
           * (1.0f + (float)c->h6 / 67108864.0f * v
              * (1.0f + (float)c->h3 / 67108864.0f * v)));
    v = v * (1.0f - (float)c->h1 * v / 524288.0f);
    /* The formula overshoots at the ends rather than saturating. */
    if (v < 0.0f) return 0.0f;
    if (v > 100.0f) return 100.0f;
    return v;
}

/* ---- the chip -------------------------------------------------------- */

#ifdef ESP_PLATFORM

#include "esp_log.h"

#define REG_ID        0xD0
#define REG_RESET     0xE0
#define REG_CTRL_HUM  0xF2
#define REG_CTRL_MEAS 0xF4
#define REG_CONFIG    0xF5
#define REG_DATA      0xF7
#define REG_CALIB_TP  0x88
#define REG_CALIB_H   0xE1

static const char *TAG = "bme280";
static i2c_master_dev_handle_t s_dev;
static i2cbus_id_t s_bus;
static bool s_present;
static uint8_t s_id;
static bme280_cal_t s_cal;

static bool read_regs(uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, n, 100) == ESP_OK;
}

static bool write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof buf, 100) == ESP_OK;
}

static bool attach(i2cbus_id_t which, uint8_t addr)
{
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(i2cbus_handle(which), &dev, &s_dev) != ESP_OK)
        return false;
    if (read_regs(REG_ID, &s_id, 1)
        && (s_id == 0x58 || s_id == 0x60 || s_id == 0x61)) {
        s_bus = which;
        return true;
    }
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return false;
}

esp_err_t bme280_init(void)
{
    /* Either bus, either address: which board it is plugged into and which
       way the module's jumper is set are questions about the desk, not the
       driver. */
    static const uint8_t addrs[2] = { 0x76, 0x77 };
    for (i2cbus_id_t which = I2CBUS_MAIN; which < I2CBUS_COUNT; which++) {
        if (i2cbus_init(which) != ESP_OK) continue;
        for (int i = 0; i < 2; i++) {
            if (!i2cbus_probe(which, addrs[i])) continue;
            if (!attach(which, addrs[i])) continue;

            uint8_t tp[26], h[7];
            if (!read_regs(REG_CALIB_TP, tp, sizeof tp)
                || !read_regs(REG_CALIB_H, h, sizeof h)) {
                ESP_LOGE(TAG, "calibration unreadable");
                return ESP_FAIL;
            }
            bme280_parse_calib(tp, h, &s_cal);

            /* Humidity oversampling has to be written before ctrl_meas: the
               chip only latches it when ctrl_meas is written afterwards, and
               setting it the other way round silently gives no humidity. */
            write_reg(REG_CTRL_HUM, 0x01);            /* humidity x1 */
            write_reg(REG_CONFIG, 0xA0);              /* 1s standby, filter off */
            write_reg(REG_CTRL_MEAS, 0x27);           /* temp x1, press x1, normal */

            s_present = true;
            ESP_LOGI(TAG, "%s at 0x%02X on the %s bus",
                     s_id == 0x60 ? "BME280" : s_id == 0x58 ? "BMP280" : "BME680",
                     addrs[i], which == I2CBUS_MAIN ? "main" : "aux");
            return ESP_OK;
        }
    }
    ESP_LOGW(TAG, "no BME280 at 0x76 or 0x77 on any bus");
    return ESP_ERR_NOT_FOUND;
}

bool bme280_present(void) { return s_present; }
uint8_t bme280_id(void) { return s_id; }
i2cbus_id_t bme280_bus(void) { return s_bus; }

bool bme280_read(float *celsius, float *hpa, float *humidity)
{
    if (!s_present) return false;
    uint8_t d[8];
    if (!read_regs(REG_DATA, d, sizeof d)) return false;

    int32_t adc_p, adc_t, adc_h;
    bme280_raw(d, &adc_p, &adc_t, &adc_h);

    /* Temperature first, always: it sets the t_fine the other two need. */
    float t = bme280_temperature(&s_cal, adc_t);
    if (celsius) *celsius = t;
    if (hpa) *hpa = bme280_pressure(&s_cal, adc_p);
    /* A BMP280 has no humidity sensor and reads 0x8000 there. */
    if (humidity) *humidity = s_id == 0x58 ? 0.0f : bme280_humidity(&s_cal, adc_h);
    return true;
}

#endif /* ESP_PLATFORM */
