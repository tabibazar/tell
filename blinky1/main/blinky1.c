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
#include "lightlink.h"
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
        ok = nvs_get_blob(h, "state", s, &n) == ESP_OK && n == sizeof *s;
        nvs_close(h);
    }
    return ok;
}

static void save(const state_t *s)
{
    nvs_handle_t h;
    if (nvs_open("light", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "state", s, sizeof *s);
    nvs_commit(h);
    nvs_close(h);
}

static void show(float r, float g, float b)
{
    led_strip_set_pixel(s_led, 0, (uint32_t)(r + 0.5f), (uint32_t)(g + 0.5f), (uint32_t)(b + 0.5f));
    led_strip_refresh(s_led);
}

static void target_of(const state_t *s, float out[3])
{
    float k = s->on ? s->level / 100.0f : 0;
    out[0] = s->r * k;
    out[1] = s->g * k;
    out[2] = s->b * k;
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

/* One reading of each sensor, to show they work. */
static void sensors_check(void)
{
    if (s_aht) {
        uint8_t st, cmd[3] = { 0xAC, 0x33, 0x00 }, d[7];
        uint8_t q = 0x71;
        i2c_master_transmit_receive(s_aht, &q, 1, &st, 1, 100);
        if ((st & 0x18) != 0x18) {                 /* not calibrated: initialise */
            uint8_t init[3] = { 0xBE, 0x08, 0x00 };
            i2c_master_transmit(s_aht, init, 3, 100);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        i2c_master_transmit(s_aht, cmd, 3, 100);
        vTaskDelay(pdMS_TO_TICKS(90));
        if (i2c_master_receive(s_aht, d, 7, 100) == ESP_OK && !(d[0] & 0x80)) {
            uint32_t rh = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
            uint32_t t = (((uint32_t)d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];
            ESP_LOGI(TAG, "AHT21: %.1f C, %.0f %% RH", t * 200.0 / 1048576.0 - 50, rh * 100.0 / 1048576.0);
        } else {
            ESP_LOGW(TAG, "AHT21: no reading");
        }
    }
    if (s_ens) {
        uint8_t reg = 0x00, id[2] = { 0 };
        i2c_master_transmit_receive(s_ens, &reg, 1, id, 2, 100);
        uint8_t mode[2] = { 0x10, 0x02 };          /* OPMODE: standard */
        i2c_master_transmit(s_ens, mode, 2, 100);
        vTaskDelay(pdMS_TO_TICKS(1500));
        uint8_t r = 0x20, d[6] = { 0 };            /* status, AQI, TVOC, eCO2 */
        i2c_master_transmit_receive(s_ens, &r, 1, d, 6, 100);
        static const char *const VAL[] = { "operating", "warming up", "first start-up", "invalid" };
        ESP_LOGI(TAG, "ENS160: part 0x%04X, %s, AQI %d, TVOC %d ppb, eCO2 %d ppm", id[0] | (id[1] << 8),
                 VAL[(d[0] >> 2) & 3], d[1] & 7, d[2] | (d[3] << 8), d[4] | (d[5] << 8));
    }
}

/* The display lit all over for a few seconds, then dark: it is alive. */
static void oled_check(void)
{
    if (!s_oled) return;
    /* 0x00 = a command stream. Charge pump on in both dialects (SSD1306's
       0x8D 0x14, SH1106's 0xAD 0x8B), display on, every pixel lit. */
    static const uint8_t on[] = { 0x00, 0xAE, 0x8D, 0x14, 0xAD, 0x8B, 0xAF, 0xA5 };
    static const uint8_t off[] = { 0x00, 0xA4, 0xAE };
    i2c_master_transmit(s_oled, on, sizeof on, 100);
    ESP_LOGI(TAG, "display: every pixel lit for 5 s");
    vTaskDelay(pdMS_TO_TICKS(5000));
    i2c_master_transmit(s_oled, off, sizeof off, 100);
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

    state_t st = { .on = 0, .r = 255, .g = 140, .b = 40, .level = 30 };
    bool had = load(&st);
    ESP_LOGI(TAG, "%s: %s rgb %u,%u,%u at %u%%", had ? "restored" : "first boot",
             st.on ? "on" : "off", st.r, st.g, st.b, st.level);

    i2c_start();
    sensors_check();
    oled_check();

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
            ESP_LOGI(TAG, "#%u from watch: %s rgb %u,%u,%u at %u%%", m.seq, m.on ? "on" : "off",
                     m.r, m.g, m.b, m.level);
        }
        int64_t now = esp_timer_get_time();
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
