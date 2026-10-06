/*
 * blinky1: the WS2812 on GPIO48, switched from watch over ESP-NOW.
 *
 * The radio listens on LIGHT_CHANNEL as a station that never joins a network.
 * A message from watch's MAC sets the colour, brightness and on/off; the LED
 * fades there over a third of a second. The last state is kept in NVS (written
 * a few seconds after the last change, to spare the flash) and comes back at
 * power-up, so the lamp survives a power cut as it was.
 */
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "airlog.h"
#include "airui.h"
#include "lightlink.h"
#include <math.h>
#include "nvs.h"
#include "nvs_flash.h"

#define LED_GPIO 48

static const char *TAG = "blinky1";
static const uint8_t WATCH_MAC[6] = LIGHT_WATCH_MAC;
static QueueHandle_t s_q;
static led_strip_handle_t s_led;

typedef struct { uint8_t on, r, g, b, level; } state_t;

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (memcmp(info->src_addr, WATCH_MAC, 6) != 0) return;   /* only watch */
    if (len != sizeof(light_msg_t)) return;
    light_msg_t m;
    memcpy(&m, data, sizeof m);
    if (m.magic != LIGHT_MAGIC || m.version != LIGHT_VERSION) return;
    xQueueSend(s_q, &m, 0);
}

static void radio_start(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    wifi_init_config_t c = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&c));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_channel(LIGHT_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_ps(WIFI_PS_NONE);              /* on USB: always listening */
    ESP_ERROR_CHECK(esp_now_init());
    esp_now_register_recv_cb(on_recv);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "listening on channel %d as " MACSTR, LIGHT_CHANNEL, MAC2STR(mac));
}

static bool load(state_t *s)
{
    nvs_handle_t h;
    size_t n = sizeof *s;
    bool ok = nvs_open("light", NVS_READONLY, &h) == ESP_OK;
    if (ok) {
        ok = nvs_get_blob(h, "state2", s, &n) == ESP_OK && n == sizeof *s;
        nvs_close(h);
    }
    return ok;
}

static void save(const state_t *s)
{
    nvs_handle_t h;
    if (nvs_open("light", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "state2", s, sizeof *s);
    nvs_commit(h);
    nvs_close(h);
}

static void show(float r, float g, float b)
{
    led_strip_set_pixel(s_led, 0, (uint32_t)(r + 0.5f), (uint32_t)(g + 0.5f), (uint32_t)(b + 0.5f));
    led_strip_refresh(s_led);
}

static air_now_t air_now(void);

/* Where the LED should be: dark, the colour watch sent, or -- the default --
   the colour of the air (airui_colour), at the brightness watch last set. */
static void target_of(const state_t *s, float out[3])
{
    uint8_t r = s->r, g = s->g, b = s->b;
    if (s->on == LIGHT_ON_AIR) {
        air_now_t n = air_now();
        airui_colour(&n, &r, &g, &b);
    }
    float k = s->on ? s->level / 100.0f : 0;
    out[0] = r * k;
    out[1] = g * k;
    out[2] = b * k;
}

/* The I2C devices, on the pins they were actually wired to (found by a pin
   search on 2026-10-03, one header position down from the plan):
     bus 0  SDA GPIO5, SCL GPIO6   ENS160 (0x53) + AHT21 (0x38) module
     bus 1  SDA GPIO7, SCL GPIO15  the display, 0x3C (an SSD1306/SH1106 OLED) */
static i2c_master_bus_handle_t s_bus[2];
static i2c_master_dev_handle_t s_aht, s_ens, s_oled;

static i2c_master_dev_handle_t dev(int bus, uint8_t addr)
{
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 100000 };
    i2c_master_dev_handle_t d = NULL;
    if (s_bus[bus] && i2c_master_probe(s_bus[bus], addr, 50) == ESP_OK)
        i2c_master_bus_add_device(s_bus[bus], &dc, &d);
    return d;
}

static void i2c_start(void)
{
    static const int sda[2] = { 5, 7 }, scl[2] = { 6, 15 };
    for (int b = 0; b < 2; b++) {
        i2c_master_bus_config_t bc = {
            .i2c_port = b, .sda_io_num = sda[b], .scl_io_num = scl[b],
            .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        if (i2c_new_master_bus(&bc, &s_bus[b]) != ESP_OK) s_bus[b] = NULL;
    }
    s_aht = dev(0, 0x38);
    s_ens = dev(0, 0x53);
    s_oled = dev(1, 0x3C);
    ESP_LOGI(TAG, "AHT21 %s, ENS160 %s, display %s", s_aht ? "found" : "MISSING",
             s_ens ? "found" : "MISSING", s_oled ? "found" : "MISSING");
}

/* ---- the sensors ------------------------------------------------------- */

static air_now_t s_now = { NAN, NAN, 0, 0, 0, 2 };
static airlog_t s_log;
static portMUX_TYPE s_now_mux = portMUX_INITIALIZER_UNLOCKED;

static air_now_t air_now(void)
{
    air_now_t n;
    portENTER_CRITICAL(&s_now_mux);
    n = s_now;
    portEXIT_CRITICAL(&s_now_mux);
    return n;
}

/* The AHT21 sits beside the ENS160's heater and the ESP32 and reads warm.
   Against Reza's hygrometer on 2026-10-06 (23.2 C, 29 %; the module read
   26.05 C, 27.7 %): the temperature 2.85 C high, and the humidity -- once
   moved from the sensor's warmer air to the room's temperature at the same
   vapour pressure (x es(raw)/es(room), 1.186 here) -- 3.8 % high. The same
   correction speaker uses (!thcal). */
#define AHT_T_OFF  (-2.85f)
#define AHT_RH_OFF (-3.84f)

static float sat_vp(float t) { return 6.112f * expf(17.62f * t / (243.12f + t)); }

static void aht_correct(float *t, float *rh)
{
    float tc = *t + AHT_T_OFF;
    float h = *rh * sat_vp(*t) / sat_vp(tc) + AHT_RH_OFF;
    *t = tc;
    *rh = h < 0 ? 0 : h > 100 ? 100 : h;
}

static bool aht_read(float *t, float *rh)
{
    if (!s_aht) return false;
    uint8_t st = 0, q = 0x71;
    i2c_master_transmit_receive(s_aht, &q, 1, &st, 1, 100);
    if ((st & 0x18) != 0x18) {                     /* not calibrated: initialise */
        uint8_t init[3] = { 0xBE, 0x08, 0x00 };
        i2c_master_transmit(s_aht, init, 3, 100);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    uint8_t cmd[3] = { 0xAC, 0x33, 0x00 }, d[7];
    if (i2c_master_transmit(s_aht, cmd, 3, 100) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(90));
    if (i2c_master_receive(s_aht, d, 7, 100) != ESP_OK || (d[0] & 0x80)) return false;
    uint32_t h = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
    uint32_t tt = (((uint32_t)d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];
    *rh = h * 100.0f / 1048576.0f;
    *t = tt * 200.0f / 1048576.0f - 50;
    return true;
}

static void ens_start(void)
{
    if (!s_ens) return;
    uint8_t mode[2] = { 0x10, 0x02 };              /* OPMODE: standard */
    i2c_master_transmit(s_ens, mode, 2, 100);
}

/* The room's temperature and humidity for the ENS160's own correction. */
static void ens_compensate(float t, float rh)
{
    if (!s_ens) return;
    uint16_t tk = (uint16_t)((t + 273.15f) * 64), h = (uint16_t)(rh * 512);
    uint8_t w[5] = { 0x13, (uint8_t)tk, (uint8_t)(tk >> 8), (uint8_t)h, (uint8_t)(h >> 8) };
    i2c_master_transmit(s_ens, w, 5, 100);
}

static bool ens_read(int *aqi, int *tvoc, int *eco2, int *validity)
{
    if (!s_ens) return false;
    uint8_t r = 0x20, d[6] = { 0 };
    if (i2c_master_transmit_receive(s_ens, &r, 1, d, 6, 100) != ESP_OK) return false;
    *validity = (d[0] >> 2) & 3;
    *aqi = d[1] & 7;
    *tvoc = d[2] | (d[3] << 8);
    *eco2 = d[4] | (d[5] << 8);
    return true;
}

static void save_log(void)
{
    static uint8_t buf[2048];
    size_t n = airlog_save(&s_log, buf, sizeof buf);
    nvs_handle_t h;
    if (n && nvs_open("air", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "log", buf, n);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void load_log(void)
{
    static uint8_t buf[2048];
    size_t n = sizeof buf;
    nvs_handle_t h;
    airlog_init(&s_log);
    if (nvs_open("air", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "log", buf, &n) == ESP_OK && airlog_load(&s_log, buf, n))
        ESP_LOGI(TAG, "air history restored: %d quarter hours, %d two-hour points", s_log.day_n, s_log.week_n);
    nvs_close(h);
}

/* Once a second: the gas sensor; every fifth, temperature and humidity.
   Minute averages go into the log. */
static void sensor_task(void *arg)
{
    (void)arg;
    ens_start();
    int64_t minute_at = esp_timer_get_time() + 60000000;
    float sum[AIR_N] = { 0 };
    int cnt[AIR_N] = { 0 };
    for (int tick = 0;; tick++) {
        float t, rh;
        if (tick % 5 == 0 && aht_read(&t, &rh)) {
            aht_correct(&t, &rh);                  /* the room's, not the module's */
            ens_compensate(t, rh);
            portENTER_CRITICAL(&s_now_mux);
            s_now.temp = t; s_now.rh = rh;
            portEXIT_CRITICAL(&s_now_mux);
            sum[AIR_TEMP] += t * 10; cnt[AIR_TEMP]++;
            sum[AIR_RH] += rh * 10; cnt[AIR_RH]++;
        }
        int aqi, tvoc, eco2, val;
        if (ens_read(&aqi, &tvoc, &eco2, &val)) {
            portENTER_CRITICAL(&s_now_mux);
            s_now.aqi = aqi; s_now.tvoc = tvoc; s_now.eco2 = eco2; s_now.validity = val;
            portEXIT_CRITICAL(&s_now_mux);
            if (val == 0) {                        /* only a warmed-up sensor's numbers are kept */
                sum[AIR_ECO2] += eco2; cnt[AIR_ECO2]++;
                sum[AIR_TVOC] += tvoc; cnt[AIR_TVOC]++;
            }
        }
        if (tick % 20 == 0) {                      /* the console too, every 20 s */
            air_now_t n = air_now();
            ESP_LOGI(TAG, "air: %.1f C, %.0f %% RH, AQI %d, TVOC %d ppb, eCO2 %d ppm, validity %d",
                     (double)n.temp, (double)n.rh, n.aqi, n.tvoc, n.eco2, n.validity);
        }
        if (esp_timer_get_time() >= minute_at) {
            minute_at += 60000000;
            air_point_t p;
            for (int k = 0; k < AIR_N; k++) {
                p.v[k] = cnt[k] ? (int16_t)lroundf(sum[k] / cnt[k]) : AIRLOG_NONE;
                sum[k] = 0; cnt[k] = 0;
            }
            if (airlog_minute(&s_log, &p)) save_log();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---- the display -------------------------------------------------------- */

#define OLED_COL_OFFSET 0       /* 2 for an SH1106 */
static uint8_t s_fb[1024];

static void oled_cmds(const uint8_t *c, size_t n)
{
    uint8_t b[32];
    b[0] = 0x00;
    memcpy(b + 1, c, n);
    i2c_master_transmit(s_oled, b, n + 1, 100);
}

static void oled_init(void)
{
    if (!s_oled) return;
    /* SSD1306, 128x64, page addressing (which an SH1106 shares). */
    static const uint8_t init[] = { 0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14,
                                    0x20, 0x02, 0xA1, 0xC8, 0xDA, 0x12, 0x81, 0x8F, 0xD9, 0xF1,
                                    0xDB, 0x40, 0xA4, 0xA6, 0xAF };
    oled_cmds(init, sizeof init);
}

static void oled_flush(void)
{
    if (!s_oled) return;
    static uint8_t b[129];
    for (int p = 0; p < 8; p++) {
        uint8_t c[3] = { (uint8_t)(0xB0 | p), (uint8_t)(0x00 | (OLED_COL_OFFSET & 0x0F)), (uint8_t)(0x10 | (OLED_COL_OFFSET >> 4)) };
        oled_cmds(c, 3);
        b[0] = 0x40;
        memcpy(b + 1, s_fb + p * 128, 128);
        i2c_master_transmit(s_oled, b, sizeof b, 100);
    }
}

/* A page every 10 s; Now redrawn every 2 s while it is up. */
static void display_task(void *arg)
{
    (void)arg;
    oled_init();
    int page = 0;
    int64_t turn_at = esp_timer_get_time() + 10000000;
    for (;;) {
        air_now_t now;
        portENTER_CRITICAL(&s_now_mux);
        now = s_now;
        portEXIT_CRITICAL(&s_now_mux);
        airui_draw(s_fb, page, &now, &s_log);
        oled_flush();
        if (esp_timer_get_time() >= turn_at) {
            turn_at += 10000000;
            page = (page + 1) % AIRUI_PAGES;
        }
        vTaskDelay(pdMS_TO_TICKS(page == 0 ? 2000 : 1000));
    }
}

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    led_strip_config_t sc = { .strip_gpio_num = LED_GPIO, .max_leds = 1,
                              .led_model = LED_MODEL_WS2812,
                              .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB };
    led_strip_rmt_config_t rc = { .resolution_hz = 10 * 1000 * 1000 };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&sc, &rc, &s_led));

    /* Air quality by default (a new key: the old saved colour was from
       before there was air to show). */
    state_t st = { .on = LIGHT_ON_AIR, .r = 0, .g = 0, .b = 0, .level = 30 };
    bool had = load(&st);
    ESP_LOGI(TAG, "%s: %s rgb %u,%u,%u at %u%%", had ? "restored" : "first boot",
             st.on == LIGHT_ON_AIR ? "air" : st.on ? "on" : "off", st.r, st.g, st.b, st.level);

    i2c_start();
    load_log();
    xTaskCreate(sensor_task, "sensors", 4096, NULL, 4, NULL);
    xTaskCreate(display_task, "display", 4096, NULL, 3, NULL);

    s_q = xQueueCreate(8, sizeof(light_msg_t));
    radio_start();

    float cur[3] = { 0, 0, 0 }, from[3], to[3];
    target_of(&st, to);
    memcpy(from, cur, sizeof from);
    int64_t fade_t0 = esp_timer_get_time(), dirty_at = 0;
    const int64_t FADE_US = 350000;
    for (;;) {
        light_msg_t m;
        if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(20)) == pdTRUE) {
            state_t n = { .on = m.on, .r = m.r, .g = m.g, .b = m.b,
                          .level = m.level < 1 ? 1 : m.level > 100 ? 100 : m.level };
            if (memcmp(&n, &st, sizeof n) != 0) {
                st = n;
                memcpy(from, cur, sizeof from);
                target_of(&st, to);
                fade_t0 = esp_timer_get_time();
                dirty_at = fade_t0 + 3000000;
            }
            ESP_LOGI(TAG, "#%u from watch: %s rgb %u,%u,%u at %u%%", m.seq,
                     m.on == LIGHT_ON_AIR ? "air" : m.on ? "on" : "off",
                     m.r, m.g, m.b, m.level);
        }
        int64_t now = esp_timer_get_time();
        static int64_t air_check;
        if (st.on == LIGHT_ON_AIR && now - air_check > 1000000) {   /* the air changes by itself */
            air_check = now;
            float nt[3];
            target_of(&st, nt);
            if (memcmp(nt, to, sizeof nt) != 0) {
                memcpy(from, cur, sizeof from);
                memcpy(to, nt, sizeof to);
                fade_t0 = now;
            }
        }
        float t = (now - fade_t0) / (float)FADE_US;
        if (t > 1) t = 1;
        float e2 = t * t * (3 - 2 * t);             /* smoothstep */
        float nc[3];
        for (int i = 0; i < 3; i++) nc[i] = from[i] + (to[i] - from[i]) * e2;
        if (memcmp(nc, cur, sizeof nc) != 0) {
            memcpy(cur, nc, sizeof cur);
            show(cur[0], cur[1], cur[2]);
        }
        if (dirty_at && now > dirty_at) {
            save(&st);
            dirty_at = 0;
        }
    }
}
