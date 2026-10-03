#include "lightlink_tx.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "lightlink.h"

static const char *TAG = "light";
static const uint8_t BLINKY1[6] = LIGHT_BLINKY1_MAC;
static bool s_inited, s_on;
static volatile light_heard_t s_heard = LIGHT_HEARD_UNKNOWN;
static uint16_t s_seq;

static void on_sent(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    s_heard = status == ESP_NOW_SEND_SUCCESS ? LIGHT_HEARD_YES : LIGHT_HEARD_NO;
}

void lightlink_radio(bool on)
{
    if (on == s_on) return;
    if (on) {
        if (!s_inited) {
            /* The WiFi driver only once the Light page is first opened: it
               costs internal RAM a watch that never uses the page keeps. */
            esp_netif_init();
            esp_event_loop_create_default();      /* ESP_ERR_INVALID_STATE if there: fine */
            wifi_init_config_t c = WIFI_INIT_CONFIG_DEFAULT();
            if (esp_wifi_init(&c) != ESP_OK) { ESP_LOGE(TAG, "no WiFi driver"); return; }
            esp_wifi_set_storage(WIFI_STORAGE_RAM);
            esp_wifi_set_mode(WIFI_MODE_STA);
            s_inited = true;
        }
        if (esp_wifi_start() != ESP_OK) return;
        esp_wifi_set_channel(LIGHT_CHANNEL, WIFI_SECOND_CHAN_NONE);
        if (esp_now_init() != ESP_OK) { esp_wifi_stop(); return; }
        esp_now_register_send_cb(on_sent);
        esp_now_peer_info_t p = { .channel = LIGHT_CHANNEL, .ifidx = WIFI_IF_STA, .encrypt = false };
        memcpy(p.peer_addr, BLINKY1, 6);
        esp_now_add_peer(&p);
        s_on = true;
        ESP_LOGI(TAG, "radio on");
    } else {
        esp_now_deinit();
        esp_wifi_stop();
        s_on = false;
        s_heard = LIGHT_HEARD_UNKNOWN;
        ESP_LOGI(TAG, "radio off");
    }
}

bool lightlink_send(uint8_t on, uint8_t r, uint8_t g, uint8_t b, uint8_t level)
{
    if (!s_on) return false;
    light_msg_t m = { .magic = LIGHT_MAGIC, .version = LIGHT_VERSION, .on = on,
                      .r = r, .g = g, .b = b, .level = level, .seq = ++s_seq };
    s_heard = LIGHT_SENDING;
    if (esp_now_send(BLINKY1, (const uint8_t *)&m, sizeof m) != ESP_OK) {
        s_heard = LIGHT_HEARD_NO;
        return false;
    }
    return true;
}

light_heard_t lightlink_heard(void) { return s_heard; }
