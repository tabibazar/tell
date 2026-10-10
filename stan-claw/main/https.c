#include "https.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "https";

int https_post(const char *url, const https_hdr_t *hdrs, int nhdrs, const char *ctype,
               const void *body, size_t len, int timeout_ms, char **resp, size_t max,
               https_sink_t sink, void *ctx)
{
    if (resp) *resp = NULL;
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_POST, .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = timeout_ms, .buffer_size = 4096, .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_http_client_set_header(c, "Content-Type", ctype);
    for (int i = 0; i < nhdrs; i++) esp_http_client_set_header(c, hdrs[i].name, hdrs[i].value);
    int status = -1;
    if (esp_http_client_open(c, (int)len) == ESP_OK) {
        size_t sent = 0;
        while (sent < len) {
            int w = esp_http_client_write(c, (const char *)body + sent, (int)(len - sent));
            if (w <= 0) break;
            sent += (size_t)w;
        }
        if (sent == len && esp_http_client_fetch_headers(c) >= 0) {
            status = esp_http_client_get_status_code(c);
            static uint8_t chunk[2048];
            char *buf = NULL;
            size_t got = 0;
            if (!sink) buf = heap_caps_malloc(max + 1, MALLOC_CAP_SPIRAM);
            int n;
            while ((n = esp_http_client_read(c, (char *)chunk, sizeof chunk)) > 0) {
                if (sink && status == 200) {
                    if (!sink(chunk, n, ctx)) break;
                } else if (buf && got < max) {
                    size_t k = (size_t)n < max - got ? (size_t)n : max - got;
                    memcpy(buf + got, chunk, k);
                    got += k;
                }
            }
            if (n < 0) status = -1;                   /* timed out mid-reply */
            if (buf) { buf[got] = 0; if (resp) *resp = buf; else free(buf); }
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (status != 200) ESP_LOGW(TAG, "%.40s...: %d", url, status);
    return status;
}
