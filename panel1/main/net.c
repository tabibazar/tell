#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "panel1_secrets.h"

static const char *TAG = "net";
static volatile bool s_up;

#define BODY_MAX (48 * 1024)   /* five rooms of a busy day are ~6 KB */

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        if (s_up) ESP_LOGW(TAG, "WiFi lost (reason %d)", d->reason);
        s_up = false;
        /* Rejoin, gently: a router that is rebooting is not helped by a
           retry every few milliseconds. */
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "on %s as " IPSTR, P1_WIFI_SSID, IP2STR(&e->ip_info.ip));
        s_up = true;
    }
}

void net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));
    wifi_config_t wc = { 0 };
    snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", P1_WIFI_SSID);
    snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", P1_WIFI_PASS);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    if (!P1_WIFI_SSID[0]) ESP_LOGE(TAG, "no WIFI_SSID in secrets/panel1.env at build time");

    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com"));
    esp_netif_sntp_init(&sc);
}

bool net_up(void) { return s_up; }

bool net_time_ok(void)
{
    return time(NULL) > 1767225600;   /* past 2026-01-01: SNTP has answered */
}

bool net_have_relay(void) { return P1_RELAY_URL[0] != 0; }

bool net_fetch_day(int y, int m, int d, rooms_day_t *out)
{
    if (!s_up || !net_have_relay()) return false;
    char url[320];
    snprintf(url, sizeof url, "%s%sd=%04d-%02d-%02d", P1_RELAY_URL,
             strchr(P1_RELAY_URL, '?') ? "&" : "?", y, m, d);
    char *body = heap_caps_malloc(BODY_MAX + 1, MALLOC_CAP_SPIRAM);
    if (!body) return false;

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,      /* Apps Script can take 20 s to wake */
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    bool ok = false;
    int got = 0, status = 0;
    /* Apps Script answers /exec with a 302 to script.googleusercontent.com,
       which holds the JSON: follow redirects by hand, as streaming wants. */
    for (int hop = 0; hop < 5; hop++) {
        if (esp_http_client_open(c, 0) != ESP_OK) break;
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            esp_http_client_set_redirection(c);
            esp_http_client_close(c);
            continue;
        }
        if (status == 200) {
            int n;
            while (got < BODY_MAX && (n = esp_http_client_read(c, body + got, BODY_MAX - got)) > 0) got += n;
            body[got] = 0;
            ok = rooms_parse(body, got, out);
        }
        break;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (!ok) ESP_LOGW(TAG, "%04d-%02d-%02d: HTTP %d, %d bytes%s%.80s", y, m, d, status, got,
                      got ? ": " : "", got ? body : "");
    else ESP_LOGI(TAG, "%04d-%02d-%02d: %d rooms, %d bytes", y, m, d, out->nrooms, got);
    free(body);
    return ok;
}
