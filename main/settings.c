#include "settings.h"

#include <string.h>

/* Values behind the choice names, in the same order. */
static const int SAVER_MIN[] = { 1, 2, 5, 10, 15, 30, 0 };
static const int DWELL_S[]   = { 10, 20, 30, 60 };

static const settings_row_t ROWS[SETTINGS_ROWS] = {
    /* A one-minute option exists so the saver can be seen to work without
       waiting ten minutes for it. */
    { "Screensaver after", { "1m", "2m", "5m", "10m", "15m", "30m", "never" }, 7 },
    { "Screensaver shows", { "cycling pages", "drifting clock" }, 2 },
    { "Seconds per page",  { "10", "20", "30", "60" }, 4 },
};

void settings_defaults(settings_t *s)
{
    s->saver_min = 10;
    s->saver_cycle = true;
    s->dwell_s = 20;
}

const settings_row_t *settings_row(int row)
{
    if (row < 0 || row >= SETTINGS_ROWS) return NULL;
    return &ROWS[row];
}

static int index_of(const int *values, int count, int value)
{
    for (int i = 0; i < count; i++)
        if (values[i] == value) return i;
    return 0;
}

int settings_choice(const settings_t *s, int row)
{
    switch (row) {
    case 0: return index_of(SAVER_MIN, ROWS[0].count, s->saver_min);
    case 1: return s->saver_cycle ? 0 : 1;
    case 2: return index_of(DWELL_S, ROWS[2].count, s->dwell_s);
    default: return 0;
    }
}

bool settings_select(settings_t *s, int row, int choice)
{
    const settings_row_t *r = settings_row(row);
    if (r == NULL || choice < 0 || choice >= r->count) return false;
    settings_t before = *s;
    switch (row) {
    case 0: s->saver_min = SAVER_MIN[choice]; break;
    case 1: s->saver_cycle = choice == 0; break;
    case 2: s->dwell_s = DWELL_S[choice]; break;
    }
    return memcmp(&before, s, sizeof *s) != 0;
}

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";
#define NAMESPACE "tell"

void settings_load(settings_t *s)
{
    settings_defaults(s);
    nvs_handle_t h;
    /* nvs_flash_init must already have run; ble_uart_start does that. */
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    /* Each key is optional, so a board flashed before a setting existed
       keeps the default for it. Values are checked against the tables so a
       stale or corrupt entry cannot select a choice that no longer exists. */
    if (nvs_get_u8(h, "saver_min", &v) == ESP_OK) {
        for (int i = 0; i < ROWS[0].count; i++)
            if (SAVER_MIN[i] == (int)v) s->saver_min = (int)v;
    }
    if (nvs_get_u8(h, "saver_cycle", &v) == ESP_OK) s->saver_cycle = v != 0;
    if (nvs_get_u8(h, "dwell_s", &v) == ESP_OK) {
        for (int i = 0; i < ROWS[2].count; i++)
            if (DWELL_S[i] == (int)v) s->dwell_s = (int)v;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "saver %dm, %s, %ds/page", s->saver_min,
             s->saver_cycle ? "cycle" : "drift", s->dwell_s);
}

bool settings_save(const settings_t *s)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed; setting not kept");
        return false;
    }
    bool ok = nvs_set_u8(h, "saver_min", (uint8_t)s->saver_min) == ESP_OK
           && nvs_set_u8(h, "saver_cycle", s->saver_cycle ? 1 : 0) == ESP_OK
           && nvs_set_u8(h, "dwell_s", (uint8_t)s->dwell_s) == ESP_OK
           && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (!ok) ESP_LOGE(TAG, "nvs write failed; setting not kept");
    return ok;
}
#else
void settings_load(settings_t *s) { settings_defaults(s); }
bool settings_save(const settings_t *s) { (void)s; return true; }
#endif
