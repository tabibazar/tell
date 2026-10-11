#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "tiny2_secrets.h"

#define NS "tiny2v"
static const char *const KEYS[SET_N] = { "k_el", "voice", "token", "volume", "speed" };
static SemaphoreHandle_t s_lock;
static set_net_t s_nets[SET_NETS];
static int s_n;
static char s_val[SET_N][SET_VAL];

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "nets", s_nets, sizeof s_nets[0] * s_n);
    for (int k = 0; k < SET_N; k++) nvs_set_str(h, KEYS[k], s_val[k]);
    nvs_set_u8(h, "seeded", 1);
    nvs_commit(h);
    nvs_close(h);
}

static void add_locked(const char *ssid, const char *pass)
{
    int i = 0;
    while (i < s_n && strcmp(s_nets[i].ssid, ssid) != 0) i++;
    if (i == SET_NETS) return;
    snprintf(s_nets[i].ssid, sizeof s_nets[i].ssid, "%s", ssid);
    snprintf(s_nets[i].pass, sizeof s_nets[i].pass, "%s", pass);
    if (i == s_n) s_n++;
}

void set_load(void)
{
    s_lock = xSemaphoreCreateMutex();
    nvs_handle_t h;
    uint8_t seeded = 0;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "seeded", &seeded);
        size_t len = sizeof s_nets;
        if (nvs_get_blob(h, "nets", s_nets, &len) == ESP_OK) s_n = (int)(len / sizeof s_nets[0]);
        for (int k = 0; k < SET_N; k++) {
            size_t l = SET_VAL;
            if (nvs_get_str(h, KEYS[k], s_val[k], &l) != ESP_OK) s_val[k][0] = 0;
        }
        nvs_close(h);
    }
    if (!seeded) {
        static const struct { const char *ssid, *pass; } nets[] = T2_SEED_NETS;
        static const char *const vals[] = T2_SEED_VALS;
        for (size_t i = 0; i < sizeof nets / sizeof nets[0]; i++) add_locked(nets[i].ssid, nets[i].pass);
        for (int k = 0; k < 3; k++) snprintf(s_val[k], SET_VAL, "%s", vals[k]);
        snprintf(s_val[SET_VOLUME], SET_VAL, "60");
        snprintf(s_val[SET_SPEED], SET_VAL, "1.0");
        save();
    }
}

int set_nets(set_net_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, s_nets, sizeof s_nets);
    int n = s_n;
    xSemaphoreGive(s_lock);
    return n;
}

bool set_net_add(const char *ssid, const char *pass)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int before = s_n;
    add_locked(ssid, pass);
    bool ok = s_n > before || strcmp(s_nets[s_n - 1].ssid, ssid) == 0;
    save();
    xSemaphoreGive(s_lock);
    return ok;
}

void set_get(set_key_t k, char *buf, size_t n)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(buf, n, "%s", s_val[k]);
    xSemaphoreGive(s_lock);
}

void set_put(set_key_t k, const char *v)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_val[k], SET_VAL, "%s", v);
    save();
    xSemaphoreGive(s_lock);
}

bool set_has(set_key_t k)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool has = s_val[k][0] != 0;
    xSemaphoreGive(s_lock);
    return has;
}
