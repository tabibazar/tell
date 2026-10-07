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
#include "freertos/event_groups.h"
#include "config.h"

static const char *TAG = "net";
static volatile bool s_up;
static char s_ssid[33];                 /* the network joined, or being tried */
static EventGroupHandle_t s_ev;
#define EV_IP    BIT0
#define EV_DOWN  BIT1

#define BODY_MAX (48 * 1024)   /* five rooms of a busy day are ~6 KB */

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        if (s_up) ESP_LOGW(TAG, "%s lost (reason %d)", s_ssid, d->reason);
        s_up = false;
        xEventGroupSetBits(s_ev, EV_DOWN);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "on %s as " IPSTR, s_ssid, IP2STR(&e->ip_info.ip));
        s_up = true;
        xEventGroupSetBits(s_ev, EV_IP);
    }
}

/* Joins the strongest known network in sight, and again whenever it drops:
   at home Tabriz, at the office the office's, with nothing to change. */
static void join_task(void *arg)
{
    static wifi_ap_record_t aps[24];
    static cfg_net_t nets[CFG_NETS];
    int quiet = 0;
    for (;;) {
        if (s_up) {
            xEventGroupWaitBits(s_ev, EV_DOWN, pdTRUE, pdFALSE, portMAX_DELAY);
            vTaskDelay(pdMS_TO_TICKS(2000));    /* a router rebooting is not helped by haste */
            continue;
        }
        int n = cfg_nets(nets);
        uint16_t na = sizeof aps / sizeof aps[0];
        if (n == 0 || esp_wifi_scan_start(NULL, true) != ESP_OK || esp_wifi_scan_get_ap_records(&na, aps) != ESP_OK) {
            if (n == 0 && quiet++ % 30 == 0) ESP_LOGW(TAG, "no networks known: 'wifi add NAME PASSWORD' on the console");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        const cfg_net_t *best = NULL;
        int rssi = -1000;
        for (int i = 0; i < na; i++)
            for (int j = 0; j < n; j++)
                if (strcmp((const char *)aps[i].ssid, nets[j].ssid) == 0 && aps[i].rssi > rssi) {
                    best = &nets[j];
                    rssi = aps[i].rssi;
                }
        if (!best) {
            if (quiet++ % 6 == 0) ESP_LOGW(TAG, "none of the %d known networks in sight (%d seen)", n, na);
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        quiet = 0;
        wifi_config_t wc = { 0 };
        memcpy(wc.sta.ssid, best->ssid, sizeof wc.sta.ssid);
        memcpy(wc.sta.password, best->pass, sizeof wc.sta.password);
        wc.sta.threshold.authmode = best->pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        wc.sta.pmf_cfg.capable = true;
        snprintf(s_ssid, sizeof s_ssid, "%s", best->ssid);
        ESP_LOGI(TAG, "joining %s (%d dBm)", s_ssid, rssi);
        xEventGroupClearBits(s_ev, EV_IP | EV_DOWN);
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        esp_wifi_connect();
        EventBits_t b = xEventGroupWaitBits(s_ev, EV_IP | EV_DOWN, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
        if (!(b & EV_IP)) {
            ESP_LOGW(TAG, "%s would not have us%s", s_ssid, (b & EV_DOWN) ? " (wrong password?)" : " (no address)");
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
        xEventGroupClearBits(s_ev, EV_DOWN);
    }
}

void net_rejoin(void)
{
    /* After the list changes: drop the link and let join_task choose again. */
    if (s_up) esp_wifi_disconnect();
}

const char *net_ssid(void) { return s_up ? s_ssid : ""; }

void net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_ev = xEventGroupCreate();
    xTaskCreatePinnedToCore(join_task, "join", 4096, NULL, 5, NULL, 0);

    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com"));
    esp_netif_sntp_init(&sc);
}

bool net_up(void) { return s_up; }

bool net_time_ok(void)
{
    return time(NULL) > 1767225600;   /* past 2026-01-01: SNTP has answered */
}

bool net_have_relay(void)
{
    char u[8];
    cfg_relay(u, sizeof u);
    return u[0] != 0;
}

bool net_fetch_day(int y, int m, int d, rooms_day_t *out)
{
    char base[320];
    cfg_relay(base, sizeof base);
    if (!s_up || !base[0]) return false;
    char url[340];
    snprintf(url, sizeof url, "%s%sd=%04d-%02d-%02d", base, strchr(base, '?') ? "&" : "?", y, m, d);
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
