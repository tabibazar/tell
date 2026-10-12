/*
 * tiny2-voice: tiny2 (an ESP32-S3 DevKitC clone) speaks. Text in -- from the
 * USB console (`say ...`) or the network (POST http://tiny2.local/say with
 * `Authorization: Bearer <token>`) -- goes to ElevenLabs, and the speech
 * streams out of GPIO14 as PWM into the amplifier wired there. One thing is
 * said at a time; the rest wait in a short queue.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"

#include "amp.h"
#include "settings.h"
#include "speak.h"

static const char *TAG = "tiny2";

#define AMP_GPIO 14
#define SAY_MAX  1000

static QueueHandle_t s_q;               /* char* (malloc'd) to say */
static volatile bool s_up;
static char s_ssid[33];

/* ---- saying ---- */

static bool enqueue(const char *text)
{
    size_t n = strnlen(text, SAY_MAX);
    char *t = malloc(n + 1);
    if (!t) return false;
    memcpy(t, text, n);
    t[n] = 0;
    if (xQueueSend(s_q, &t, 0) != pdTRUE) { free(t); return false; }
    return true;
}

static void say_task(void *arg)
{
    for (;;) {
        char *t;
        xQueueReceive(s_q, &t, portMAX_DELAY);
        if (strcmp(t, "\x01tone") == 0) {
            speak_tone();
        } else {
            /* Just restarted (opening the USB port resets it): give WiFi a moment. */
            for (int i = 0; i < 40 && !s_up; i++) vTaskDelay(pdMS_TO_TICKS(500));
            int code = s_up ? speak(t) : -1;
            if (code != 200) ESP_LOGW(TAG, "could not say it: %s", code == 0 ? "ElevenLabs key missing"
                                      : code == -2 ? "voice missing" : code < 0 ? "no network" : "ElevenLabs error");
            else ESP_LOGI(TAG, "said: %.60s", t);
        }
        free(t);
    }
}

/* ---- WiFi: the strongest known network in sight ---- */

static void join(void)
{
    set_net_t nets[SET_NETS];
    int n = set_nets(nets);
    wifi_scan_config_t sc = { 0 };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return;
    uint16_t found = 20;
    wifi_ap_record_t *aps = calloc(found, sizeof *aps);
    if (!aps) return;
    esp_wifi_scan_get_ap_records(&found, aps);
    int best = -1, rssi = -200;
    for (int i = 0; i < found; i++)
        for (int k = 0; k < n; k++)
            if (strcmp((char *)aps[i].ssid, nets[k].ssid) == 0 && aps[i].rssi > rssi) { best = k; rssi = aps[i].rssi; }
    free(aps);
    if (best < 0) { ESP_LOGW(TAG, "no known network in sight"); return; }
    wifi_config_t wc = { 0 };
    memcpy(wc.sta.ssid, nets[best].ssid, sizeof wc.sta.ssid);           /* both fit exactly: 32 and 64 + NUL */
    memcpy(wc.sta.password, nets[best].pass, sizeof wc.sta.password);
    snprintf(s_ssid, sizeof s_ssid, "%s", nets[best].ssid);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
}

static void net_task(void *arg)
{
    for (;;) {
        if (!s_up) join();
        vTaskDelay(pdMS_TO_TICKS(s_up ? 5000 : 10000));
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) s_up = false;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_up = true;
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "on %s as " IPSTR " (http://tiny2.local/say)", s_ssid, IP2STR(&e->ip_info.ip));
    }
}

/* ---- POST /say ---- */

static bool authorised(httpd_req_t *r)
{
    char got[SET_VAL + 16] = "", want[SET_VAL + 16], tok[SET_VAL];
    set_get(SET_TOKEN, tok, sizeof tok);
    snprintf(want, sizeof want, "Bearer %s", tok);
    httpd_req_get_hdr_value_str(r, "Authorization", got, sizeof got);
    return tok[0] && strcmp(got, want) == 0;
}

static esp_err_t h_say(httpd_req_t *r)
{
    if (!authorised(r)) { httpd_resp_set_status(r, "401 Unauthorized"); return httpd_resp_send(r, "token?\n", HTTPD_RESP_USE_STRLEN); }
    if (r->content_len <= 0 || r->content_len > SAY_MAX) {
        httpd_resp_set_status(r, "400 Bad Request");
        return httpd_resp_send(r, "send 1-1000 bytes of text\n", HTTPD_RESP_USE_STRLEN);
    }
    char *body = malloc(r->content_len + 1);
    int got = 0;
    while (body && got < r->content_len) {
        int k = httpd_req_recv(r, body + got, r->content_len - got);
        if (k <= 0) break;
        got += k;
    }
    if (!body || got != r->content_len) { free(body); return httpd_resp_send_500(r); }
    body[got] = 0;
    bool ok = enqueue(body);
    free(body);
    if (!ok) { httpd_resp_set_status(r, "503 Service Unavailable"); return httpd_resp_send(r, "busy\n", HTTPD_RESP_USE_STRLEN); }
    return httpd_resp_send(r, "ok\n", HTTPD_RESP_USE_STRLEN);
}

static void http_start(void)
{
    httpd_handle_t h;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&h, &cfg) != ESP_OK) return;
    httpd_uri_t say = { .uri = "/say", .method = HTTP_POST, .handler = h_say };
    httpd_register_uri_handler(h, &say);
    mdns_init();
    mdns_hostname_set("tiny2");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

/* ---- the console ---- */

static void run(char *line)
{
    while (*line == ' ') line++;
    if (strncmp(line, "say ", 4) == 0) {
        printf(enqueue(line + 4) ? "ok: saying it\n" : "no: busy\n");
    } else if (strcmp(line, "tone") == 0) {
        printf(enqueue("\x01tone") ? "ok: chime\n" : "no: busy\n");
    } else if (strncmp(line, "volume ", 7) == 0) {
        int v = atoi(line + 7);
        amp_volume(v);
        char b[8];
        snprintf(b, sizeof b, "%d", amp_get_volume());
        set_put(SET_VOLUME, b);
        printf("ok: volume %d\n", amp_get_volume());
    } else if (strncmp(line, "speed ", 6) == 0) {
        const char *v = strcmp(line + 6, "slower") == 0 ? "0.85" : strcmp(line + 6, "faster") == 0 ? "1.15" : "1.0";
        set_put(SET_SPEED, v);
        printf("ok: speed %s\n", v);
    } else if (strncmp(line, "voice ", 6) == 0) {
        set_put(SET_VOICE, line + 6);
        printf("ok: voice set\n");
    } else if (strncmp(line, "key ", 4) == 0) {
        set_put(SET_ELEVEN_KEY, line + 4);
        printf("ok: key set\n");
    } else if (strncmp(line, "token ", 6) == 0) {
        set_put(SET_TOKEN, line + 6);
        printf("ok: token set\n");
    } else if (strncmp(line, "wifi add ", 9) == 0) {
        char *ssid = line + 9, *pass = strchr(ssid, ' ');
        if (pass) *pass++ = 0;
        printf(set_net_add(ssid, pass ? pass : "") ? "ok: network added\n" : "no: full\n");
    } else if (strcmp(line, "status") == 0) {
        char sp[16];
        set_get(SET_SPEED, sp, sizeof sp);
        printf("wifi: %s\nelevenlabs key: %s\nvoice: %s\ntoken: %s\nvolume: %d\nspeed: %s\n",
               s_up ? s_ssid : "not joined", set_has(SET_ELEVEN_KEY) ? "set" : "not set",
               set_has(SET_VOICE) ? "set" : "not set", set_has(SET_TOKEN) ? "set" : "not set", amp_get_volume(), sp);
    } else if (line[0]) {
        printf("commands: say TEXT | tone | volume 0-100 | speed slower|normal|faster | voice ID | key K | "
               "token T | wifi add SSID PASS | status\n");
    }
}

static void console_task(void *arg)
{
    uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
    static char line[SAY_MAX + 16];
    size_t n = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r' || c == '\n') {
            line[n] = 0;
            run(line);
            n = 0;
        } else if (n + 1 < sizeof line) {
            line[n++] = (char)c;
        }
    }
}

void app_main(void)
{
    /* First of all: the amplifier's input low, not floating (a floating input hisses). */
    gpio_reset_pin(AMP_GPIO);
    gpio_set_direction(AMP_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(AMP_GPIO, 0);
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
    set_load();
    char v[SET_VAL];
    set_get(SET_VOLUME, v, sizeof v);
    amp_volume(v[0] ? atoi(v) : 60);
    if (amp_init(AMP_GPIO) != ESP_OK) ESP_LOGE(TAG, "no audio");

    s_q = xQueueCreate(4, sizeof(char *));
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wcfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    http_start();

    xTaskCreatePinnedToCore(say_task, "say", 12288, NULL, 5, NULL, 1);     /* TLS */
    xTaskCreate(net_task, "net", 4096, NULL, 4, NULL);
    xTaskCreate(console_task, "console", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "ready: `say hello` on the console, or POST /say");
}
