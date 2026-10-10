#include "config.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "stanclaw_secrets.h"

static const char *TAG = "config";
#define NS "stanclaw"

static const char *const KEYS[CFG_N] = { "k_claude", "k_dg", "k_el", "voice", "model", "mcp_url", "mcp_tok", "srv_tok",
                                          "voice_nm", "volume", "speed" };
static SemaphoreHandle_t s_lock;
static cfg_net_t s_nets[CFG_NETS];
static int s_n;
static char s_val[CFG_N][CFG_VAL];

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "nets", s_nets, sizeof s_nets[0] * s_n);
    for (int k = 0; k < CFG_N; k++) nvs_set_str(h, KEYS[k], s_val[k]);
    nvs_set_u8(h, "seeded", 1);
    nvs_commit(h);
    nvs_close(h);
}

static void add_locked(const char *ssid, const char *pass)
{
    int i = 0;
    while (i < s_n && strcmp(s_nets[i].ssid, ssid) != 0) i++;
    if (i == CFG_NETS) return;
    snprintf(s_nets[i].ssid, sizeof s_nets[i].ssid, "%s", ssid);
    snprintf(s_nets[i].pass, sizeof s_nets[i].pass, "%s", pass);
    if (i == s_n) s_n++;
}

/* panel1 kept its networks as a blob of {ssid[33], pass[65]} in namespace
   "panel1" -- the same layout as cfg_net_t. */
static void import_panel1(void)
{
    nvs_handle_t h;
    if (nvs_open("panel1", NVS_READONLY, &h) != ESP_OK) return;
    cfg_net_t nets[CFG_NETS];
    size_t len = sizeof nets;
    if (nvs_get_blob(h, "nets", nets, &len) == ESP_OK)
        for (size_t i = 0; i < len / sizeof nets[0]; i++) add_locked(nets[i].ssid, nets[i].pass);
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
        for (int k = 0; k < CFG_N; k++) {
            len = CFG_VAL;
            if (nvs_get_str(h, KEYS[k], s_val[k], &len) != ESP_OK) s_val[k][0] = 0;
        }
        nvs_close(h);
    }
    if (!seeded) {
        static const struct { const char *ssid, *pass; } seed[] = SC_SEED_NETS;
        for (size_t i = 0; i < sizeof seed / sizeof seed[0] && seed[i].ssid; i++) add_locked(seed[i].ssid, seed[i].pass);
        import_panel1();
        static const char *const vals[CFG_N] = SC_SEED_VALS;
        for (int k = 0; k < CFG_N; k++) snprintf(s_val[k], CFG_VAL, "%s", vals[k] ? vals[k] : "");   /* the later keys have no seed */
        save();
        ESP_LOGI(TAG, "seeded: %d network(s)", s_n);
    }
    ESP_LOGI(TAG, "%d network(s); keys: claude %s, deepgram %s, elevenlabs %s; mcp %s", s_n,
             s_val[CFG_CLAUDE_KEY][0] ? "set" : "missing", s_val[CFG_DEEPGRAM_KEY][0] ? "set" : "missing",
             s_val[CFG_ELEVEN_KEY][0] ? "set" : "missing", s_val[CFG_MCP_URL][0] ? "set" : "none");
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
    int before = s_n;
    bool known = false;
    for (int i = 0; i < s_n; i++) known |= strcmp(s_nets[i].ssid, ssid) == 0;
    add_locked(ssid, pass);
    bool ok = known || s_n > before;
    if (ok) save();
    xSemaphoreGive(s_lock);
    return ok;
}

bool cfg_net_forget(const char *ssid)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = false;
    for (int i = 0; i < s_n && !found; i++) {
        if (strcmp(s_nets[i].ssid, ssid) == 0) {
            memmove(&s_nets[i], &s_nets[i + 1], sizeof s_nets[0] * (s_n - i - 1));
            s_n--;
            found = true;
        }
    }
    if (found) save();
    xSemaphoreGive(s_lock);
    return found;
}

void cfg_get(cfg_key_t k, char *buf, size_t n)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *v = s_val[k];
    if (k == CFG_MODEL && !v[0]) v = "claude-sonnet-5";
    snprintf(buf, n, "%s", v);
    xSemaphoreGive(s_lock);
}

void cfg_set(cfg_key_t k, const char *v)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_val[k], CFG_VAL, "%s", v);
    save();
    xSemaphoreGive(s_lock);
}

bool cfg_has(cfg_key_t k)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool has = s_val[k][0] != 0;
    xSemaphoreGive(s_lock);
    return has;
}
