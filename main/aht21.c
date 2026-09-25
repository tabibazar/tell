#include "aht21.h"

#include <stdio.h>
#include <string.h>

uint8_t aht21_crc(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

void aht21_convert(const uint8_t d[6], float *celsius, float *humidity)
{
    /* Twenty bits each, sharing d[3]: humidity takes its high nibble and
       temperature its low one. Reading the shared byte the wrong way round
       is the mistake this driver exists to not make. */
    uint32_t raw_h = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4)
                   | ((uint32_t)d[3] >> 4);
    uint32_t raw_t = (((uint32_t)d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8)
                   | (uint32_t)d[5];

    /* Both are fractions of a full twenty-bit scale: humidity over 0..100,
       temperature over -50..150. */
    if (humidity) {
        float rh = (float)raw_h * 100.0f / 1048576.0f;
        if (rh < 0.0f) rh = 0.0f;
        if (rh > 100.0f) rh = 100.0f;
        *humidity = rh;
    }
    if (celsius) *celsius = (float)raw_t * 200.0f / 1048576.0f - 50.0f;
}

void aht21_tally_note(aht21_tally_t *t, int64_t now_us, const uint8_t frame[7],
                      bool crc_ok)
{
    if (!t->started) {
        t->started = true;
        t->since_us = now_us;
    }
    t->reads++;
    if (!crc_ok) {
        t->bad++;
        memcpy(t->last_bad, frame, sizeof t->last_bad);
    }
}

bool aht21_tally_report(aht21_tally_t *t, int64_t now_us, char *out, int size)
{
    if (!t->started || now_us - t->since_us < AHT21_REPORT_US) return false;

    /* The real span rather than a fixed "10": a board that slept through
       part of it read less often, and the count only means something beside
       the time it was counted over. */
    int minutes = (int)((now_us - t->since_us) / (60LL * 1000000));
    bool any = t->bad > 0;
    if (any && out && size > 0) {
        const uint8_t *d = t->last_bad;
        snprintf(out, (size_t)size,
                 "%d of %d reads failed CRC in the last %d min; newest bad frame "
                 "%02x %02x %02x %02x %02x %02x crc %02x want %02x",
                 t->bad, t->reads, minutes,
                 d[0], d[1], d[2], d[3], d[4], d[5], d[6], aht21_crc(d, 6));
    }
    t->since_us = now_us;
    t->reads = 0;
    t->bad = 0;
    return any;
}

bool aht21_vet_trusted(const aht21_vet_t *v)
{
    int bad = 0;
    for (int i = 0; i < v->frames; i++) bad += (v->failed >> i) & 1;
    return bad * 2 <= v->frames;
}

static bool close_to(float a, float b, float within)
{
    float d = a - b;
    return d <= within && -d <= within;
}

aht21_verdict_t aht21_vet(aht21_vet_t *v, const uint8_t d[7],
                          float *celsius, float *humidity)
{
    float c = 0, rh = 0;
    bool sound = aht21_crc(d, 6) == d[6] && (d[0] & AHT21_STATUS_CAL);
    if (sound) {
        aht21_convert(d, &c, &rh);
        sound = c >= AHT21_MIN_C && c <= AHT21_MAX_C;
    }
    v->failed = (uint16_t)((v->failed << 1) | (sound ? 0u : 1u));
    if (v->frames < AHT21_HISTORY) v->frames++;

    /* The busy bit is read only from a frame that is otherwise sound: in a
       damaged one it is no more believable than the rest. Either way this
       frame is no neighbour for the next. */
    if (!sound || (d[0] & AHT21_STATUS_BUSY)) {
        v->prev_ok = false;
        return AHT21_DROP;
    }

    bool agrees = v->prev_ok && close_to(c, v->prev_c, AHT21_AGREE_C)
                             && close_to(rh, v->prev_rh, AHT21_AGREE_RH);
    v->prev_ok = true;
    v->prev_c = c;
    v->prev_rh = rh;

    /* Distrusted frames are still remembered, so that a sensor coming back
       has a neighbour ready the moment the rate recovers. */
    if (!aht21_vet_trusted(v)) return AHT21_DROP;
    if (!agrees) return AHT21_CONFIRM;
    if (celsius) *celsius = c;
    if (humidity) *humidity = rh;
    return AHT21_USE;
}

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "aht21";
static i2c_master_dev_handle_t s_dev;
static i2cbus_id_t s_bus;
static bool s_present;
static aht21_tally_t s_tally;

static bool tx(const uint8_t *d, size_t n)
{
    return i2c_master_transmit(s_dev, d, n, 100) == ESP_OK;
}

static bool rx(uint8_t *d, size_t n)
{
    return i2c_master_receive(s_dev, d, n, 100) == ESP_OK;
}

esp_err_t aht21_init(void)
{
    s_present = false;
    for (i2cbus_id_t which = I2CBUS_MAIN; which < I2CBUS_COUNT; which++) {
        if (i2cbus_init(which) != ESP_OK) continue;
        if (!i2cbus_probe(which, AHT21_ADDR)) continue;

        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = AHT21_ADDR,
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(i2cbus_handle(which), &dev, &s_dev) != ESP_OK)
            continue;

        /* The chip calibrates itself at power-on and says so in its status.
           Only nudge it if it has not: an unnecessary init costs 10 ms and a
           needless write to a part that was already ready. */
        uint8_t st = 0;
        if (rx(&st, 1) && !(st & AHT21_STATUS_CAL)) {
            const uint8_t init[3] = { 0xBE, 0x08, 0x00 };
            tx(init, sizeof init);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        s_bus = which;
        s_present = true;
        ESP_LOGI(TAG, "AHT21 at 0x%02X on the %s bus", AHT21_ADDR,
                 which == I2CBUS_MAIN ? "main" : "aux");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "no AHT21 at 0x%02X on any bus", AHT21_ADDR);
    return ESP_ERR_NOT_FOUND;
}

bool aht21_present(void) { return s_present; }
i2cbus_id_t aht21_bus(void) { return s_bus; }

static aht21_vet_t s_vet;

/* One measurement: the command, the chip's own conversion time, the frame. */
static bool measure(uint8_t d[7])
{
    const uint8_t cmd[3] = { 0xAC, 0x33, 0x00 };
    if (!tx(cmd, sizeof cmd)) return false;
    vTaskDelay(pdMS_TO_TICKS(80));
    return rx(d, 7);
}

bool aht21_read(float *celsius, float *humidity)
{
    if (!s_present) return false;

    /*
     * The CRC covers the status and both readings, and a frame that fails it
     * is thrown away whole -- temperature as well as humidity, since both came
     * in the same damaged bytes. Passing it is not enough on its own either:
     * aht21_vet says why, and what else a frame must pass. At most two
     * measurements, the second only when the first is sound but has no sound
     * neighbour to agree with.
     *
     * CRC failures are counted rather than warned about one by one: see
     * aht21_tally_t. Distrust is announced when it starts and when it ends,
     * so a log shows why a sensor answering with good CRCs gave no reading.
     */
    for (int attempt = 0; attempt < 2; attempt++) {
        uint8_t d[7];
        if (!measure(d)) return false;

        int64_t now = esp_timer_get_time();
        aht21_tally_note(&s_tally, now, d, aht21_crc(d, 6) == d[6]);
        char line[128];
        if (aht21_tally_report(&s_tally, now, line, sizeof line))
            ESP_LOGW(TAG, "%s", line);

        bool trusted = aht21_vet_trusted(&s_vet);
        float c, rh;
        aht21_verdict_t verdict = aht21_vet(&s_vet, d, &c, &rh);
        if (aht21_vet_trusted(&s_vet) != trusted)
            ESP_LOGW(TAG, "%s", trusted
                     ? "most recent frames failed; none will be used until that changes"
                     : "most recent frames are sound again; readings resume");

        if (verdict == AHT21_USE) {
            if (celsius) *celsius = c;
            if (humidity) *humidity = rh;
            return true;
        }
        if (verdict == AHT21_DROP) return false;
    }
    return false;
}
#endif
