#include "where.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "where";

static char s_body[512];
static int s_body_len;

static esp_err_t on_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && s_body_len + e->data_len < (int)sizeof s_body - 1) {
        memcpy(s_body + s_body_len, e->data, e->data_len);
        s_body_len += e->data_len;
        s_body[s_body_len] = 0;
    }
    return ESP_OK;
}

bool where_lookup(const net_cell_t *c, double *lat, double *lon, int *accuracy_m)
{
    if (!c->ok) return false;
    char req[256];
    snprintf(req, sizeof req,
             "{\"considerIp\":false,\"cellTowers\":[{\"radioType\":\"lte\",\"mobileCountryCode\":%d,"
             "\"mobileNetworkCode\":%d,\"locationAreaCode\":%d,\"cellId\":%ld,\"signalStrength\":%d}]}",
             c->mcc, c->mnc, c->tac, c->cell, c->rsrp);
    esp_http_client_config_t cfg = {
        .url = "https://api.beacondb.net/v1/geolocate",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_event,
        .user_agent = "tabibazar-spy/1.0",
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return false;
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_post_field(h, req, (int)strlen(req));
    s_body_len = 0;
    s_body[0] = 0;
    esp_err_t e = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (e != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "beacondb: %s, HTTP %d %.120s", esp_err_to_name(e), status, s_body);
        return false;
    }
    cJSON *root = cJSON_Parse(s_body);
    cJSON *loc = root ? cJSON_GetObjectItem(root, "location") : NULL;
    cJSON *la = loc ? cJSON_GetObjectItem(loc, "lat") : NULL, *lo = loc ? cJSON_GetObjectItem(loc, "lng") : NULL;
    cJSON *acc = root ? cJSON_GetObjectItem(root, "accuracy") : NULL;
    bool ok = cJSON_IsNumber(la) && cJSON_IsNumber(lo);
    if (ok) {
        *lat = la->valuedouble;
        *lon = lo->valuedouble;
        *accuracy_m = cJSON_IsNumber(acc) ? (int)acc->valuedouble : -1;
        ESP_LOGI(TAG, "cell %ld: %.5f, %.5f (+-%d m)", c->cell, *lat, *lon, *accuracy_m);
    }
    cJSON_Delete(root);
    return ok;
}
