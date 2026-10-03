#include "camlink_tx.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "camlink.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "camlink";
static const uint8_t TINY1[6] = CAMLINK_TINY1_MAC;
static void (*s_on_ask)(void);
static QueueHandle_t s_q;
static SemaphoreHandle_t s_sent;
static volatile bool s_ok, s_heard;
static uint16_t s_frame;

typedef struct { char path[64]; uint8_t hour, minute, weekday; } job_t;

static void on_sent(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    s_ok = status == ESP_NOW_SEND_SUCCESS;
    xSemaphoreGive(s_sent);
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (memcmp(info->src_addr, TINY1, 6) != 0 || len < (int)sizeof(camlink_hdr_t)) return;
    camlink_hdr_t h;
    memcpy(&h, data, sizeof h);
    if (h.magic == CAMLINK_MAGIC && h.type == CAMLINK_ASK && s_on_ask) s_on_ask();
}

static bool send_chunk(const uint8_t *buf, size_t len)
{
    for (int tries = 0; tries < 3; tries++) {
        xSemaphoreTake(s_sent, 0);
        if (esp_now_send(TINY1, buf, len) != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        if (xSemaphoreTake(s_sent, pdMS_TO_TICKS(200)) == pdTRUE && s_ok) return true;
    }
    return false;
}

static void tx_task(void *arg)
{
    (void)arg;
    uint8_t *jpg = heap_caps_malloc(CAMLINK_MAX, MALLOC_CAP_SPIRAM);
    uint8_t *pkt = heap_caps_malloc(sizeof(camlink_hdr_t) + CAMLINK_CHUNK, MALLOC_CAP_INTERNAL);
    for (;;) {
        job_t j;
        if (xQueueReceive(s_q, &j, portMAX_DELAY) != pdTRUE || !jpg || !pkt) continue;
        FILE *f = fopen(j.path, "rb");
        if (!f) continue;
        size_t n = fread(jpg, 1, CAMLINK_MAX, f);
        bool whole = feof(f);
        fclose(f);
        if (!whole || n == 0) { ESP_LOGW(TAG, "%s too big for tiny1", j.path); continue; }
        camlink_hdr_t h = { .magic = CAMLINK_MAGIC, .type = CAMLINK_PART, .hour = j.hour, .minute = j.minute,
                            .weekday = j.weekday, .frame = ++s_frame, .total = (uint32_t)n };
        bool ok = true;
        for (size_t off = 0; off < n && ok; off += CAMLINK_CHUNK) {
            h.offset = (uint32_t)off;
            h.len = (uint16_t)(n - off < CAMLINK_CHUNK ? n - off : CAMLINK_CHUNK);
            memcpy(pkt, &h, sizeof h);
            memcpy(pkt + sizeof h, jpg + off, h.len);
            ok = send_chunk(pkt, sizeof h + h.len);
        }
        if (ok != s_heard) ESP_LOGI(TAG, "tiny1 %s", ok ? "is listening" : "is not answering");
        s_heard = ok;
    }
}

bool camlink_start(void (*on_ask)(void))
{
    s_on_ask = on_ask;
    s_q = xQueueCreate(2, sizeof(job_t));
    s_sent = xSemaphoreCreateBinary();
    /* The WiFi driver, as a station that never joins anything: ESP-NOW
       needs the radio, and hotspot.c adds the access point to it on
       request (APSTA) rather than owning it. */
    esp_netif_init();
    esp_event_loop_create_default();
    wifi_init_config_t c = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&c) != ESP_OK) return false;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (esp_wifi_start() != ESP_OK) return false;
    esp_wifi_set_channel(CAMLINK_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) return false;
    /* The radio asleep between short wake windows: listening all the time
       would cost the 18650 tens of mA. tiny1 repeats its asks until one
       lands in a window; sends wake the radio themselves. */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    esp_wifi_connectionless_module_set_wake_interval(100);
    esp_now_set_wake_window(25);
    esp_now_register_send_cb(on_sent);
    esp_now_register_recv_cb(on_recv);
    esp_now_peer_info_t p = { .channel = CAMLINK_CHANNEL, .ifidx = WIFI_IF_STA, .encrypt = false };
    memcpy(p.peer_addr, TINY1, 6);
    esp_now_add_peer(&p);
    xTaskCreatePinnedToCore(tx_task, "camlink", 4096, NULL, 3, NULL, 1);
    ESP_LOGI(TAG, "up on channel %d", CAMLINK_CHANNEL);
    return true;
}

void camlink_send_file(const char *path, int hour, int minute, int weekday)
{
    if (!s_q) return;
    job_t j = { .hour = (uint8_t)hour, .minute = (uint8_t)minute, .weekday = (uint8_t)weekday };
    snprintf(j.path, sizeof j.path, "%s", path);
    xQueueSend(s_q, &j, 0);                 /* full: tiny1 gets the next one */
}

bool camlink_heard(void) { return s_heard; }
