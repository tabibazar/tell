#include "config.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "panel1_secrets.h"

static const char *TAG = "config";
#define NS "panel1"

static SemaphoreHandle_t s_lock;
static cfg_net_t s_nets[CFG_NETS];
static int s_n;
static char s_relay[320];

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "nets", s_nets, sizeof s_nets[0] * s_n);
    nvs_set_str(h, "relay", s_relay);
    nvs_set_u8(h, "seeded", 1);
    nvs_commit(h);
    nvs_close(h);
}

void cfg_load(void)
{
    s_lock = xSemaphoreCreateMutex();
    nvs_handle_t h;
    uint8_t seeded = 0;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "seeded", &seeded);
        size_t len = sizeof s_nets;
        if (nvs_get_blob(h, "nets", s_nets, &len) == ESP_OK) s_n = len / sizeof s_nets[0];
        len = sizeof s_relay;
        if (nvs_get_str(h, "relay", s_relay, &len) != ESP_OK) s_relay[0] = 0;
        nvs_close(h);
    }
    if (!seeded) {
        /* First boot: what was built in. */
        static const struct { const char *ssid, *pass; } seed[] = P1_SEED_NETS;
        for (size_t i = 0; i < sizeof seed / sizeof seed[0] && seed[i].ssid; i++)
            cfg_net_add(seed[i].ssid, seed[i].pass);
        snprintf(s_relay, sizeof s_relay, "%s", P1_RELAY_URL);
        save();
        ESP_LOGI(TAG, "seeded from the build: %d network(s), relay %s", s_n, s_relay[0] ? "set" : "none");
    }
    ESP_LOGI(TAG, "%d network(s) known, relay %s", s_n, s_relay[0] ? "set" : "not set");
}

int cfg_nets(cfg_net_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_n;
    memcpy(out, s_nets, sizeof s_nets[0] * n);
    xSemaphoreGive(s_lock);
    return n;
}

bool cfg_net_add(const char *ssid, const char *pass)
{
    if (!ssid[0] || strlen(ssid) > 32 || strlen(pass) > 64) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = 0;
    while (i < s_n && strcmp(s_nets[i].ssid, ssid) != 0) i++;
    bool ok = i < CFG_NETS;
    if (ok) {
        snprintf(s_nets[i].ssid, sizeof s_nets[i].ssid, "%s", ssid);
        snprintf(s_nets[i].pass, sizeof s_nets[i].pass, "%s", pass);
        if (i == s_n) s_n++;
        save();
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool cfg_net_forget(const char *ssid)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = false;
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_nets[i].ssid, ssid) == 0) {
            memmove(&s_nets[i], &s_nets[i + 1], sizeof s_nets[0] * (s_n - i - 1));
            s_n--;
            found = true;
            break;
        }
    }
    if (found) save();
    xSemaphoreGive(s_lock);
    return found;
}

void cfg_relay(char *buf, size_t n)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(buf, n, "%s", s_relay);
    xSemaphoreGive(s_lock);
}

void cfg_set_relay(const char *url)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_relay, sizeof s_relay, "%s", url);
    save();
    xSemaphoreGive(s_lock);
}
