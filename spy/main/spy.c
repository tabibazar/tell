/* spy: milestone 1 -- 4G up, the time, and a message to Telegram. */
#include <stdio.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "net.h"
#include "tg.h"

static const char *TAG = "spy";

static void on_text(const char *text, int64_t date)
{
    ESP_LOGI(TAG, "message at %lld: %s", (long long)date, text);
}

void app_main(void)
{
    nvs_flash_init();
    ESP_LOGI(TAG, "internal free %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (!net_up()) { ESP_LOGE(TAG, "no network"); return; }
    ESP_LOGI(TAG, "internal free after PPP %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    char msg[96];
    time_t now = time(NULL);
    struct tm lt; localtime_r(&now, &lt);
    strftime(msg, sizeof msg, "spy online over 4G, %a %H:%M", &lt);
    ESP_LOGI(TAG, "send: %d", tg_send_text(msg));
    ESP_LOGI(TAG, "internal free after TLS %u, min %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    int64_t off = -1;
    for (int i = 0; i < 3; i++) ESP_LOGI(TAG, "poll %d: %d next %lld", i, tg_poll(&off, 10, on_text), (long long)off);
}
