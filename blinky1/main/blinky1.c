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
