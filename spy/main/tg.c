#include "tg.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "spy_secrets.h"

static const char *TAG = "tg";
#define BOUNDARY "spyFormBoundary7MA4YWxkTrZu0gW"
#define RESP_CAP (24 * 1024)

bool tg_configured(void) { return SPY_TG_TOKEN[0] != 0 && SPY_TG_CHAT[0] != 0; }

/* No buzzing phone between 21:00 and 07:00: messages still arrive, silently. */
static bool quiet_hours(void)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    return lt.tm_year > 100 && (lt.tm_hour >= 21 || lt.tm_hour < 7);
}

static esp_http_client_handle_t client_for(const char *method, int timeout_ms)
{
    char url[160];
    snprintf(url, sizeof url, "https://api.telegram.org/bot%s/%s", SPY_TG_TOKEN, method);
    esp_http_client_config_t c = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .keep_alive_enable = false,
    };
    return esp_http_client_init(&c);
}

/* Reads the reply after the body is sent. True for HTTP 200 with "ok":true;
   the reply's text lands in `resp` (if given) for the caller to parse. */
static bool finish(esp_http_client_handle_t h, const char *method, char *resp, size_t cap)
{
    if (esp_http_client_fetch_headers(h) < 0) {
        ESP_LOGW(TAG, "%s: no reply", method);
        return false;
    }
    int status = esp_http_client_get_status_code(h);
    char small[512];
    char *buf = resp ? resp : small;
    size_t lim = resp ? cap : sizeof small;
    int got = 0, n;
    while (got < (int)lim - 1 && (n = esp_http_client_read(h, buf + got, lim - 1 - got)) > 0) got += n;
    buf[got] = 0;
    bool ok = status == 200 && strstr(buf, "\"ok\":true");
    if (!ok) ESP_LOGW(TAG, "%s: HTTP %d %.200s", method, status, buf);
    return ok;
}

static bool post_json(const char *method, const char *json, char *resp, size_t cap, int timeout_ms)
{
    esp_http_client_handle_t h = client_for(method, timeout_ms);
    if (!h) return false;
    esp_http_client_set_header(h, "Content-Type", "application/json");
    size_t len = strlen(json);
    bool ok = esp_http_client_open(h, len) == ESP_OK &&
              esp_http_client_write(h, json, len) == (int)len &&
              finish(h, method, resp, cap);
    esp_http_client_cleanup(h);
    return ok;
}

bool tg_send_text(const char *text)
{
    if (!tg_configured()) return false;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "chat_id", SPY_TG_CHAT);
    cJSON_AddStringToObject(o, "text", text);
    if (quiet_hours()) cJSON_AddBoolToObject(o, "disable_notification", true);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    bool ok = json && post_json("sendMessage", json, NULL, 0, 30000);
    free(json);
    return ok;
}

static int part(char *out, size_t cap, const char *name, const char *value)
{
    return snprintf(out, cap, "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n",
                    name, value);
}

bool tg_send_file(const char *method, const char *field, const char *path,
                  const char *mime, const char *caption, const char *extra)
{
    if (!tg_configured()) return false;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        ESP_LOGW(TAG, "%s: no file %s", method, path);
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    /* Everything before the file's bytes, then the bytes, then the closing
       boundary: the length is known up front, so it streams from the card. */
    char *head = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    char *chunk = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!head || !chunk) { free(head); free(chunk); fclose(f); return false; }
    int n = part(head, 2048, "chat_id", SPY_TG_CHAT);
    if (caption && caption[0]) n += part(head + n, 2048 - n, "caption", caption);
    if (quiet_hours()) n += part(head + n, 2048 - n, "disable_notification", "true");
    while (extra && *extra) {                  /* "name=value\n" lines */
        const char *eq = strchr(extra, '='), *nl = strchr(extra, '\n');
        if (!eq) break;
        if (!nl) nl = extra + strlen(extra);
        char name[32], value[64];
        snprintf(name, sizeof name, "%.*s", (int)(eq - extra), extra);
        snprintf(value, sizeof value, "%.*s", (int)(nl - eq - 1), eq + 1);
        n += part(head + n, 2048 - n, name, value);
        extra = *nl ? nl + 1 : nl;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    n += snprintf(head + n, 2048 - n,
                  "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
                  "Content-Type: %s\r\n\r\n", field, base, mime);
    static const char tail[] = "\r\n--" BOUNDARY "--\r\n";
    size_t total = (size_t)n + (size_t)st.st_size + sizeof tail - 1;

    /* ~11 KB/s over PPP at 115200: allow for that and then some. */
    int timeout = 30000 + (int)(st.st_size / 4);
    esp_http_client_handle_t h = client_for(method, timeout);
    bool ok = false;
    if (h) {
        esp_http_client_set_header(h, "Content-Type", "multipart/form-data; boundary=" BOUNDARY);
        if (esp_http_client_open(h, total) == ESP_OK && esp_http_client_write(h, head, n) == n) {
            size_t sent = 0, r;
            ok = true;
            while ((r = fread(chunk, 1, 8192, f)) > 0) {
                if (esp_http_client_write(h, chunk, r) != (int)r) { ok = false; break; }
                sent += r;
            }
            ok = ok && sent == (size_t)st.st_size &&
                 esp_http_client_write(h, tail, sizeof tail - 1) == (int)(sizeof tail - 1) &&
                 finish(h, method, NULL, 0);
        }
        esp_http_client_cleanup(h);
    }
    ESP_LOGI(TAG, "%s %s (%ld bytes): %s", method, base, (long)st.st_size, ok ? "sent" : "FAILED");
    fclose(f);
    free(head);
    free(chunk);
    return ok;
}

bool tg_poll(int64_t *offset, int timeout_s, void (*on_text)(const char *text, int64_t date))
{
    if (!tg_configured()) return false;
    char json[128];
    snprintf(json, sizeof json, "{\"offset\":%" PRId64 ",\"timeout\":%d,\"allowed_updates\":[\"message\"]}",
             *offset, timeout_s);
    char *resp = heap_caps_malloc(RESP_CAP, MALLOC_CAP_SPIRAM);
    if (!resp) return false;
    bool ok = post_json("getUpdates", json, resp, RESP_CAP, (timeout_s + 20) * 1000);
    if (ok) {
        cJSON *root = cJSON_Parse(resp);
        cJSON *res = root ? cJSON_GetObjectItem(root, "result") : NULL;
        cJSON *u;
        cJSON_ArrayForEach(u, res) {
            cJSON *id = cJSON_GetObjectItem(u, "update_id");
            if (cJSON_IsNumber(id) && (int64_t)id->valuedouble >= *offset)
                *offset = (int64_t)id->valuedouble + 1;
            cJSON *m = cJSON_GetObjectItem(u, "message");
            cJSON *chat = m ? cJSON_GetObjectItem(m, "chat") : NULL;
            cJSON *cid = chat ? cJSON_GetObjectItem(chat, "id") : NULL;
            cJSON *text = m ? cJSON_GetObjectItem(m, "text") : NULL;
            cJSON *date = m ? cJSON_GetObjectItem(m, "date") : NULL;
            if (!cJSON_IsNumber(cid) || !cJSON_IsString(text)) continue;
            char cs[24];
            snprintf(cs, sizeof cs, "%" PRId64, (int64_t)cid->valuedouble);
            if (strcmp(cs, SPY_TG_CHAT) != 0) {
                ESP_LOGW(TAG, "ignoring a message from chat %s", cs);   /* only ours */
                continue;
            }
            on_text(text->valuestring, cJSON_IsNumber(date) ? (int64_t)date->valuedouble : 0);
        }
        cJSON_Delete(root);
    }
    free(resp);
    return ok;
}
