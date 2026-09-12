#include "wifi.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "wifi";
static bool s_ready;

esp_err_t wifi_start(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * Station mode, started, and deliberately never told to connect. Nothing
     * here holds a password, and a driver with an association in flight will
     * not scan -- so not joining is what makes the scanning reliable as well
     * as what makes it work in someone else's house.
     */
    esp_wifi_set_mode(WIFI_MODE_STA);
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    ESP_LOGI(TAG, "listening (2.4 GHz only; 5 GHz networks are invisible)");
    return ESP_OK;
}

static int by_strength(const void *a, const void *b)
{
    const wifi_ap_t *x = a, *y = b;
    return y->rssi - x->rssi;
}

int wifi_scan(wifi_ap_t *out, int max)
{
    if (!s_ready || max <= 0) return 0;

    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) { esp_wifi_clear_ap_list(); return 0; }
    if (found > (uint16_t)max) found = (uint16_t)max;

    /* A record is over 200 bytes and internal memory is the scarce thing
       here, so these come from PSRAM where there is any. */
    wifi_ap_record_t *recs = heap_caps_calloc(found, sizeof *recs,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (recs == NULL) recs = calloc(found, sizeof *recs);
    if (recs == NULL) { esp_wifi_clear_ap_list(); return 0; }

    uint16_t n = found;
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) { free(recs); return 0; }
    for (uint16_t i = 0; i < n; i++) {
        strncpy(out[i].ssid, (const char *)recs[i].ssid, sizeof out[i].ssid - 1);
        out[i].ssid[sizeof out[i].ssid - 1] = '\0';
        out[i].rssi = recs[i].rssi;
        out[i].channel = recs[i].primary;
    }
    free(recs);
    qsort(out, n, sizeof *out, by_strength);
    return (int)n;
}
