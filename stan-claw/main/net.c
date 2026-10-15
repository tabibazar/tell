#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
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
static char s_seen[16][33];             /* the last scan's networks, strongest first */
static volatile int s_nseen;
static volatile bool s_none_known;
static volatile int s_fails;            /* joins refused in a row (a changed password) */
static char s_prefer[33];
static esp_netif_t *s_ap;

/* The scan's networks, strongest first, each name once, for the setup page. */
static void remember(const wifi_ap_record_t *aps, int na)
{
    int order[24], k = 0;
    for (int i = 0; i < na && i < 24; i++) order[k++] = i;
    for (int a = 1; a < k; a++)
        for (int b = a; b > 0 && aps[order[b]].rssi > aps[order[b - 1]].rssi; b--) {
            int t = order[b]; order[b] = order[b - 1]; order[b - 1] = t;
        }
    int n = 0;
    for (int i = 0; i < k && n < 16; i++) {
        const char *name = (const char *)aps[order[i]].ssid;
        bool dup = !name[0];
        for (int j = 0; j < n && !dup; j++) dup = strcmp(s_seen[j], name) == 0;
        if (!dup) snprintf(s_seen[n++], sizeof s_seen[0], "%s", name);
    }
    s_nseen = n;
}
#define EV_IP    BIT0
#define EV_DOWN  BIT1

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
        s_fails = 0;
        s_up = true;
        xEventGroupSetBits(s_ev, EV_IP);
    }
}

/* Joins the strongest known network in sight, and again whenever it drops:
   at home the home one, at the office the office's, with nothing to change. */
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
        if (esp_wifi_scan_start(NULL, true) != ESP_OK || esp_wifi_scan_get_ap_records(&na, aps) != ESP_OK) {
            xEventGroupWaitBits(s_ev, EV_DOWN, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));   /* or sooner: net_rejoin */
            continue;
        }
        remember(aps, na);
        if (n == 0) {
            s_none_known = true;
            if (quiet++ % 30 == 0) ESP_LOGW(TAG, "no networks known: set one up from a phone, or 'wifi add' on the console");
            xEventGroupWaitBits(s_ev, EV_DOWN, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));   /* or sooner: net_rejoin */
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
        /* The network just set up from a phone goes first, if it is in sight. */
        for (int i = 0; s_prefer[0] && i < na; i++)
            for (int j = 0; j < n; j++)
                if (strcmp((const char *)aps[i].ssid, s_prefer) == 0 && strcmp(nets[j].ssid, s_prefer) == 0) {
                    best = &nets[j];
                    rssi = aps[i].rssi;
                }
        s_prefer[0] = 0;
        s_none_known = !best;
        if (!best) {
            if (quiet++ % 6 == 0) ESP_LOGW(TAG, "none of the %d known networks in sight (%d seen)", n, na);
            xEventGroupWaitBits(s_ev, EV_DOWN, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));   /* or sooner: net_rejoin */
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
            s_fails++;
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
        xEventGroupClearBits(s_ev, EV_DOWN);
    }
}

void net_setup_start(const char *ssid, const char *pass)
{
    if (!s_ap) s_ap = esp_netif_create_default_wifi_ap();
    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof ap.ap.ssid, "%s", ssid);
    ap.ap.ssid_len = (uint8_t)strlen(ssid);
    snprintf((char *)ap.ap.password, sizeof ap.ap.password, "%s", pass);
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 4;
    ap.ap.channel = 6;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    ESP_LOGI(TAG, "setup network %s up at 192.168.4.1", ssid);
}

void net_setup_stop(void)
{
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "setup network down");
}

int net_seen(char names[][33], int max)
{
    if (!s_nseen) {                       /* joined, so never scanned: look now */
        static wifi_ap_record_t aps[24];
        uint16_t na = sizeof aps / sizeof aps[0];
        if (esp_wifi_scan_start(NULL, true) == ESP_OK && esp_wifi_scan_get_ap_records(&na, aps) == ESP_OK) remember(aps, na);
    }
    int n = s_nseen < max ? s_nseen : max;
    for (int i = 0; i < n; i++) memcpy(names[i], s_seen[i], 33);       /* both 33: a name and its NUL */
    return n;
}

bool net_none_known(void) { return s_none_known || s_fails >= 3; }

void net_prefer(const char *ssid) { snprintf(s_prefer, sizeof s_prefer, "%s", ssid); }

void net_rejoin(void)
{
    /* After the list changes: drop the link and let join_task choose again. */
    if (s_up) esp_wifi_disconnect();
    else xEventGroupSetBits(s_ev, EV_DOWN);
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
